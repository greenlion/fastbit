// File $Id$
// Author: John Wu <John.Wu@ACM.org> Lawrence Berkeley National Laboratory
// Copyright 2000-2008 the Regents of the University of California
//
///@File
///
/// Implementation of the ibis::part functions except a few that modify the
/// content of the partition (which are in parti.cpp) or perform self
/// join (in party.cpp).
///
#if defined(_WIN32) && defined(_MSC_VER)
#pragma warning(disable:4786)	// some identifier longer than 256 characters
#endif

#include "qExpr.h"
#include "category.h"
#include "query.h"
#include "part.h"
#include "iroster.h"

#include <fstream>
#include <sstream>	// std::ostringstream
#include <algorithm>	// std::find, std::less, ...
#include <typeinfo>	// typeid
#include <stdio.h>
#include <stdlib.h>	// RAND_MAX, rand()
#include <stdarg.h>	// vsprintf(), ...
#include <signal.h>
#include <ctype.h>	// tolower
#include <cmath>	// std::floor, std::ceil
#if defined(sun)
#include <ieeefp.h>
#elif defined(_WIN32)
#include <float.h>	// DBL_MAX, _finite
#endif
#if defined(_WIN32) && !defined(__CYGWIN__)
#define popen _popen
#define pclose _pclose
#elif defined(HAVE_DIRENT_H)
#  include <dirent.h>
#endif

extern "C" {
    // a function to start the some test queries
    static void* ibis_part_startTests(void* arg) {
	if (arg == 0)
	    return reinterpret_cast<void*>(-1L);
	ibis::part::thrArg* myArg = (ibis::part::thrArg*)arg;
	if (myArg->et == 0)
	    return reinterpret_cast<void*>(-2L);
	const ibis::part *et0 = myArg->et;
	LOGGER(3) << "INFO: starting a new thread to perform "
		"self-test on partition " << et0->name();

	try {
	    std::string longtest;
	    ibis::part::readLock lock(et0, "startTests");
	    if (myArg->pref) {
		longtest = myArg->pref;
		longtest += ".longTests";
	    }
	    else {
		longtest = et0->name();
		longtest += ".longTests";
	    }

	    if (et0->nRows() < 1048576 ||
		ibis::gParameters().isTrue(longtest.c_str()))
		et0->queryTest(myArg->pref, myArg->nerrors);
	    else
		et0->quickTest(myArg->pref, myArg->nerrors);
	    return(reinterpret_cast<void*>(0L));
	}
	catch (const std::exception& e) {
	    et0->logMessage("startTests",
			    "self test received exception \"%s\"",
			    e.what());
	    return(reinterpret_cast<void*>(-11L));
	}
	catch (const char* s) {
	    et0->logMessage("startTests",
			    "self test received exception \"%s\"",
			    s);
	    return(reinterpret_cast<void*>(-12L));
	}
	catch (...) {
	    et0->logMessage("startTests", "self test received an unexpected "
			    "exception");
	    return(reinterpret_cast<void*>(-10L));
	}
    } // ibis_part_startTests

    // the routine wraps around doBackup to allow doBackup being run in a
    // separated thread
    static void* ibis_part_startBackup(void* arg) {
	if (arg == 0) return reinterpret_cast<void*>(-1L);
	ibis::part* et = (ibis::part*)arg; // arg is actually a part*
	try {
	    ibis::part::readLock lock(et, "startBackup");
	    et->doBackup();
	    return(reinterpret_cast<void*>(0L));
	}
	catch (const std::exception& e) {
	    et->logMessage("startBackup", "doBackup received "
			   "exception \"%s\"", e.what());
	    return(reinterpret_cast<void*>(-21L));
	}
	catch (const char* s) {
	    et->logMessage("startBackup", "doBackup received "
			   "exception \"%s\"", s);
	    return(reinterpret_cast<void*>(-22L));
	}
	catch (...) {
	    et->logMessage("startBackup", "doBackup received "
			   "an unexpected exception");
	    return(reinterpret_cast<void*>(-20L));
	}
    } // ibis_part_startBackup

    static void* ibis_part_build_index(void* arg) {
	if (arg == 0) return reinterpret_cast<void*>(-1L);
	ibis::part::indexBuilderPool &pool =
	    *(reinterpret_cast<ibis::part::indexBuilderPool*>(arg));
	try {
	    for (uint32_t i = pool.cnt(); i < pool.tbl.nColumns();
		 i = pool.cnt()) {
		ibis::column *col = pool.tbl.getColumn(i);
		if (col != 0) {
		    if (! (col->upperBound() >= col->lowerBound()))
			col->computeMinMax();
		    col->loadIndex(pool.opt);
		    col->unloadIndex();
		    char *fnm = col->dataFileName();
		    ibis::fileManager::instance().flushFile(fnm);
		    delete [] fnm;
		}
	    }
	    return 0;
	}
	catch (const std::exception& e) {
	    pool.tbl.logMessage("buildIndex", "loadIndex "
				"received std::exception \"%s\"", e.what());
	    return(reinterpret_cast<void*>(-31L));
	}
	catch (const char* s) {
	    pool.tbl.logMessage("buildIndex", "loadIndex "
				"received exception \"%s\"", s);
	    return(reinterpret_cast<void*>(-32L));
	}
	catch (...) {
	    pool.tbl.logMessage("buildIndex", "loadIndex received an "
				"unexpected exception");
	    return(reinterpret_cast<void*>(-30L));
	}
    } // ibis_part_build_index
} // extern "C"

/// The default prefix (NULL) tells it to use names in global namespace of
/// the resource list (ibis::gParameters).  When a valid string is
/// specified, it looks for data directory names under
/// 'prefix.dataDir1=aaaa' and 'prefix.dataDir2=bbbb'.
ibis::part::part(const char* prefix) :
    m_name(0), m_desc(0), rids(0), nEvents(0), activeDir(0),
    backupDir(0), switchTime(0), state(UNKNOWN_STATE), idxstr(0),
    myCleaner(0) {
    // initialize the locks
    if (0 != pthread_mutex_init
	(&mutex, static_cast<const pthread_mutexattr_t*>(0))) {
	throw "ibis::part unable to initialize the mutex lock";
    }

    if (0 != pthread_rwlock_init(&rwlock, 0)) {
	throw "ibis::part unable to initialize the rwlock";
    }

    // for the special "incore" data partition, there is no need to call
    // the function init
    if (stricmp(prefix, "incore") != 0)
	init(prefix);
} // the default constructor of ibis::part

/// The meta tags are specified as a list of name-value strings, where each
/// string in one name-value pair.
ibis::part::part(const std::vector<const char*>& mtags) :
    m_name(0), m_desc(0), rids(0), nEvents(0), activeDir(0),
    backupDir(0), switchTime(0), state(UNKNOWN_STATE), idxstr(0),
    myCleaner(0) {
    // initialize the locks
    if (0 != pthread_mutex_init
	(&mutex, static_cast<const pthread_mutexattr_t*>(0))) {
	throw "ibis::part unable to initialize the mutex lock";
    }

    if (0 != pthread_rwlock_init(&rwlock, 0)) {
	throw "ibis::part unable to initialize the rwlock";
    }

    std::string pref;
    genName(mtags, pref);
    init(pref.c_str());
    if (mtags.size() > 2 || 0 == stricmp(mtags[0], "name"))
	setMetaTags(mtags);
} // constructor from a vector of strings

/// The name-value pairs are specified in a structured form.
ibis::part::part(const ibis::resource::vList& mtags) :
    m_name(0), m_desc(0), rids(0), nEvents(0), activeDir(0),
    backupDir(0), switchTime(0), state(UNKNOWN_STATE), idxstr(0),
    myCleaner(0) {
    // initialize the locks
    if (0 != pthread_mutex_init
	(&mutex, static_cast<const pthread_mutexattr_t*>(0))) {
	throw "ibis::part unable to initialize the mutex lock";
    }

    if (0 != pthread_rwlock_init(&rwlock, 0)) {
	throw "ibis::part unable to initialize the rwlock";
    }

    std::string pref; // new name
    genName(mtags, pref);
    init(pref.c_str());
    if (mtags.size() > 1 || mtags.find("name") != mtags.begin())
	setMetaTags(mtags);
} // constructor from vList

/// Construct a partition from the named directories.  Originally, FastBit
/// was designed to work with a pair of directories, @c adir and @c bdir.
/// Normally, data is stored only in one directory, specify the directory
/// as @c adir and leave @c bdir as null.
ibis::part::part(const char* adir, const char* bdir) :
    m_name(0), m_desc(0), rids(0), nEvents(0), activeDir(0),
    backupDir(0), switchTime(0), state(UNKNOWN_STATE), idxstr(0),
    myCleaner(0) {
    (void) ibis::fileManager::instance(); // initialize the file manager
    // initialize the locks
    if (pthread_mutex_init(&mutex, 0)) {
	throw "ibis::part::ctor unable to initialize the mutex lock";
    }

    if (pthread_rwlock_init(&rwlock, 0)) {
	throw "ibis::part::ctor unable to initialize the rwlock";
    }

    if (adir == 0) return;
    //if (*adir != DIRSEP) return;
    activeDir = ibis::util::strnewdup(adir);
    ibis::util::removeTail(activeDir, DIRSEP);
    { // make sure activeDir exists, extra block to limit the scope of tmp
	Stat_T tmp;
	if (UnixStat(activeDir, &tmp) == 0) {
	    if ((tmp.st_mode&S_IFDIR) != S_IFDIR) {
		ibis::util::logMessage("Warning", "ibis::part::ctor(%s): "
				       "stat.st_mode=%d is not a directory",
				       adir, static_cast<int>(tmp.st_mode));
		throw "ibis::part::ctor the given name is not a directory";
	    }
	}
	else {
	    if (ibis::gVerbose > 5 || errno != ENOENT)
		ibis::util::logMessage("Warning", "ibis::part::ctor(%s): "
				       "stat(%s) failed ... %s", adir, adir,
				       strerror(errno));
	    if (errno == ENOENT) { // no such directory, make one
		int ierr = ibis::util::makeDir(adir);
		if (ierr < 0)
		    throw "Can NOT generate the necessary directory for data";

		activeDir = ibis::util::strnewdup(adir);
		myCleaner = new ibis::part::cleaner(this);
		ibis::fileManager::instance().addCleaner(myCleaner);
		return;
	    }
	    else {
		throw "ibis::part::ctor -- named directory does not exist.";
	    }
	}
    }

    // read the metadata file in activeDir
    int maxLength = readTDC(nEvents, columns, activeDir);
    if (maxLength > 0) {
	// read in the RIDs
	readRIDs();
	if (rids->size() > 0 && rids->size() != nEvents)
	    nEvents = rids->size();
	if (nEvents > 0 && switchTime == 0)
	    switchTime = time(0);
    }

    if (m_name == 0 && nEvents > 0) {
	// copy the directory name as the name of the data part
	char* tmp = strrchr(activeDir, DIRSEP);
	if (tmp != 0)
	    m_name = ibis::util::strnewdup(tmp+1);
	else
	    m_name = ibis::util::strnewdup(activeDir);
    }
    // generate the backupDir name
    if (bdir != 0 && *bdir != 0) {
	char *tmp = backupDir;
	backupDir = const_cast<char*>(bdir); // will not modify bdir
	if (0 == verifyBackupDir()) { // no obvious error
	    backupDir = ibis::util::strnewdup(bdir);
	    delete [] tmp;
	}
	else { // restore the backupDir read from metadata file
	    backupDir = tmp;
	    if (ibis::gVerbose > 0)
		ibis::util::logMessage
		    ("Warning", "user provided directory \"%s\" doesn't match "
		     "the active data directory \"%s\"; use the alternative "
		     "directory \"%s\" stored in the metadata file",
		     bdir, activeDir, tmp);
	}
    }
    if (backupDir == 0) {
	std::string nm = "ibis.table";
	if (m_name != 0) {
	    nm += '.';
	    nm += m_name;
	}
	nm += ".useBackupDir";
	const char *str = ibis::gParameters()[nm.c_str()];
	if (str == 0) {
	    nm.erase(nm.size()-9);
	    nm += "ShadowDir";
	    str = ibis::gParameters()[nm.c_str()];
	}
	if (ibis::resource::isStringTrue(str)) {
	    if (bdir)
		backupDir = ibis::util::strnewdup(bdir);
	    else
		deriveBackupDirName();
	}
    }

    if (nEvents > 0) { // read mask of the partition
	std::string mskfile(activeDir);
	if (! mskfile.empty())
	    mskfile += DIRSEP;
	mskfile += "-part.msk";
	try {
	    amask.read(mskfile.c_str());
	    if (amask.size() != nEvents) {
		if (ibis::gVerbose > 1 && amask.size() > 0)
		    ibis::util::logMessage
			("Warning", "mask file \"%s\" contains only %lu "
			 "bits but %lu was expected", mskfile.c_str(),
			 static_cast<long unsigned>(amask.size()),
			 static_cast<long unsigned>(nEvents));
		amask.adjustSize(nEvents, nEvents);
		if (amask.cnt() < nEvents) {
		    amask.write(mskfile.c_str());
		}
		else {
		    remove(mskfile.c_str());
		}
		ibis::fileManager::instance().flushFile(mskfile.c_str());
	    }
	}
	catch (...) {
	    if (ibis::gVerbose > 1)
		ibis::util::logMessage
		    ("Warning", "unable to read mask file \"%s\", assume "
		     "all %lu rows are active", mskfile.c_str(),
		     static_cast<long unsigned>(nEvents));
	    amask.set(1, nEvents);
	    remove(mskfile.c_str());
	}
    }

    // superficial checks
    int j = 0;
    if (maxLength <= 0) maxLength = 16;
    if (strlen(activeDir)+16+maxLength > PATH_MAX) {
	ibis::util::logMessage("Warning", "directory name \"%s\" is too "
			       "long", activeDir);
	++j;
    }
    if (backupDir != 0 && strlen(backupDir)+16+maxLength > PATH_MAX) {
	ibis::util::logMessage("Warning", "directory name \"%s\" is too "
			       "long", backupDir);
	++j;
    }
    if (j) throw "direcotry names too long";

    myCleaner = new ibis::part::cleaner(this);
    ibis::fileManager::instance().addCleaner(myCleaner);

    if (ibis::gVerbose > 0 && m_name != 0) {
	ibis::util::logger lg(0);
	lg.buffer() << "Completed construction of an ";
	if (nEvents == 0)
	    lg.buffer() << "empty ";
	lg.buffer() << "ibis::part named "
		    << (m_name?m_name:"<NULL>");
	if (m_desc != 0 && *m_desc != 0)
	    lg.buffer() << " (" << m_desc << ")";
	lg.buffer() << "\n";
	if (ibis::gVerbose > 1) {
	    lg.buffer() << "activeDir = \"" << activeDir << "\"\n";
	    if (backupDir != 0)
		lg.buffer() << "backupDir = \"" << backupDir << "\"\n";
	}
	if (ibis::gVerbose > 3 && columns.size() > 0) {
	    print(lg.buffer());
	}
	else if (nEvents > 0 && ! columns.empty()) {
	    lg.buffer() << nEvents << " rows each with "
			<< columns.size() << " columns\n";
	}
    }
} // construct part from the named direcotries

// the destructor
ibis::part::~part() {
    {   // make sure all read accesses have finished
	writeLock lock(this, "~part");
	if (ibis::gVerbose > 3 && m_name != 0)
	    ibis::util::logMessage("~part", "clearing partition %s",
				   m_name);

	// Because the key in the std::map that defined the columns are
	// part of the objects to be deleted, need to copy the columns into
	// a linear container to avoid any potential problems.
	std::vector<ibis::column*> tmp;
	tmp.reserve(columns.size());
	for (columnList::const_iterator it = columns.begin();
	     it != columns.end(); ++ it)
	    tmp.push_back((*it).second);
	columns.clear();

	for (uint32_t i = 0; i < tmp.size(); ++ i)
	    delete tmp[i];
	tmp.clear();
    }

    ibis::fileManager::instance().removeCleaner(myCleaner);
    ibis::resource::clear(metaList);
    // clear the rid list and the rest
    delete rids;
    delete myCleaner;
    if (backupDir != 0 && *backupDir != 0)
	ibis::fileManager::instance().flushDir(backupDir);
    //ibis::fileManager::instance().flushDir(activeDir);
    delete [] activeDir;
    delete [] backupDir;
    delete [] idxstr;
    delete [] m_desc;
    delete [] m_name;

    pthread_mutex_destroy(&mutex);
    pthread_rwlock_destroy(&rwlock);
} // the destructor

void ibis::part::genName(const std::vector<const char*>& mtags,
			 std::string &name) {
    for (uint32_t i = 1; i < mtags.size(); i += 2) {
	if (i > 1)
	    name += '_';
	name += mtags[i];
    }
    if (name.empty())
	name = ibis::util::userName();
} // ibis::part::genName

void ibis::part::genName(const ibis::resource::vList& mtags,
			 std::string &name) {
    bool isStar = (mtags.size() == 3);
    if (isStar) {
	isStar = ((mtags.find("trgSetupName") != mtags.end()) &&
		  (mtags.find("production") != mtags.end()) &&
		  (mtags.find("magScale") != mtags.end()));
    }
    if (isStar) {
	ibis::resource::vList::const_iterator it = mtags.find("production");
	name = (*it).second;
	name += '_';
	it = mtags.find("trgSetupName");
	name += (*it).second;
	name += '_';
	it = mtags.find("magScale");
	name += (*it).second;
    }
    else {
	for (ibis::resource::vList::const_iterator it = mtags.begin();
	     it != mtags.end(); ++it) {
	    if (it != mtags.begin())
		name += '_';
	    name += (*it).second;
	}
    }
    if (name.empty())
	name = ibis::util::userName();
} // ibis::part::genName

/// Determines where to store the data.
void ibis::part::init(const char* prefix) {
    // make sure the file manager is initialized
    (void) ibis::fileManager::instance();

    int j = 0;
    if (prefix != 0 && *prefix != 0)
	j = strlen(prefix);
    char* fn = new char[j+64];
    strcpy(fn, "ibis.");
    if (prefix != 0 && *prefix != 0) {
	strcpy(fn+5, prefix);
	fn[j+5] = '.';
    }
    j += 6;

    // get the active directory name
    const char* str;
    strcpy(fn+j, "activeDir");
    str = ibis::gParameters()[fn];
    if (str != 0) {
	activeDir = ibis::util::strnewdup(str);
	strcpy(fn+j, "backupDir");
	str = ibis::gParameters()[fn];
	if (str != 0 && *str != 0)
	    backupDir = ibis::util::strnewdup(str);
    }
    else {
	strcpy(fn+j, "DataDir1");
	str = ibis::gParameters()[fn];
	if (str != 0) {
	    activeDir = ibis::util::strnewdup(str);
	    strcpy(fn+j, "DataDir2");
	    str = ibis::gParameters()[fn];
	    if (str != 0 && *str != 0)
		backupDir = ibis::util::strnewdup(str);
	}
	else {
	    strcpy(fn+j, "activeDirectory");
	    str = ibis::gParameters()[fn];
	    if (str != 0) {
		activeDir = ibis::util::strnewdup(str);
		strcpy(fn+j, "backupDirectory");
		str = ibis::gParameters()[fn];
		if (str != 0 && *str != 0)
		    backupDir = ibis::util::strnewdup(str);
	    }
	    else {
		strcpy(fn+j, "DataDir");
		str = ibis::gParameters()[fn];
		if (str != 0) {
		    activeDir = ibis::util::strnewdup(str);
		    strcpy(fn+j, "backupDir");
		    str = ibis::gParameters()[fn];
		    if (str != 0 && *str != 0)
			backupDir = ibis::util::strnewdup(str);
		}
		else {
		    strcpy(fn+j, "DataDirectory");
		    str = ibis::gParameters()[fn];
		    if (str != 0) {
			activeDir = ibis::util::strnewdup(str);
			strcpy(fn+j, "backupDirectory");
			str = ibis::gParameters()[fn];
			if (str != 0 && *str != 0)
			    backupDir = ibis::util::strnewdup(str);
		    }
		    else {
			strcpy(fn+j, "IndexDirectory");
			str = ibis::gParameters()[fn];
			if (str != 0) {
			    activeDir = ibis::util::strnewdup(str);
			}
			else {
			    strcpy(fn+j, "DataDir2");
			    str = ibis::gParameters()[fn];
			    if (0 != str) {
				backupDir = ibis::util::strnewdup(str);
			    }
			}
		    }
		}
	    }
	}
    }
    if (activeDir == 0) {
	if (DIRSEP == '/') {
	    activeDir = ibis::util::strnewdup(".ibis/dir1");
	}
	else {
	    activeDir = ibis::util::strnewdup(".ibis\\dir1");
	}
    }

    ibis::util::removeTail(activeDir, DIRSEP);
    try {
	int ierr = ibis::util::makeDir(activeDir); // make sure it exists
	if (ierr < 0)
	    throw "Can NOT generate the necessary data directory";
    }
    catch (const std::exception& e) {
	LOGGER(0) << "Warning -- " << e.what();
	throw "ibis::part::init received a std::exception while calling "
	    "makeDir";
    }
    catch (const char* s) {
	LOGGER(0) << "Warning -- " << s;
	throw "ibis::part::init received a string exception while calling "
	    "makeDir";
    }
    catch (...) {
	throw "ibis::part::init received a unknown exception while "
	    "calling makeDir";
    }

    // read metadata file in activeDir
    int maxLength = readTDC(nEvents, columns, activeDir);
    const char *tmp = strrchr(activeDir, DIRSEP);
    const bool useDir = (maxLength <=0 &&
			 (prefix == 0 || *prefix == 0 ||
			  (tmp != 0 ?			     
			   (0 == strcmp(tmp+1, prefix)) :
			   (0 == strcmp(activeDir, prefix)))));
    if (! useDir) { // need a new subdirectory
	std::string subdir = activeDir;
	subdir += DIRSEP;
	subdir += prefix;
	ibis::util::makeDir(subdir.c_str());
	delete [] activeDir;
	activeDir = ibis::util::strnewdup(subdir.c_str());
	maxLength = readTDC(nEvents, columns, activeDir);
	if (backupDir != 0) {
	    subdir = backupDir;
	    subdir += DIRSEP;
	    subdir += prefix;
	    int ierr = ibis::util::makeDir(subdir.c_str());
	    delete [] backupDir;
	    if (ierr >= 0)
		backupDir = ibis::util::strnewdup(subdir.c_str());
	    else
		backupDir = 0;
	}
    }
    if (maxLength > 0) { // metadata file exists
	// read in the RIDs
	readRIDs();
	if (rids->size() > 0 && rids->size() != nEvents)
	    nEvents = rids->size();
	if (nEvents > 0 && switchTime == 0)
	    switchTime = time(0);

	std::string fillrids(m_name);
	fillrids += ".fillRIDs";
	if (nEvents > 0 && ibis::gParameters().isTrue(fillrids.c_str())) {
	    std::string fname(activeDir);
	    fname += DIRSEP;
	    fname += "rids";
	    fillRIDs(fname.c_str());
	}
    }

    delete [] fn;

    if (m_name == 0) { // should assign a name
	if (prefix) {  // use argument to this function
	    m_name = ibis::util::strnewdup(prefix);
	}
	else if (nEvents > 0) {
	    // use the directory name
	    if (tmp != 0)
		m_name = ibis::util::strnewdup(tmp+1);
	    else
		m_name = ibis::util::strnewdup(activeDir);
	}
    }

    if (backupDir != 0 &&
	0 == strncmp(backupDir, activeDir, strlen(backupDir)))
	deriveBackupDirName();

    if (backupDir != 0) {
	ibis::util::removeTail(backupDir, DIRSEP);
	// verify its header file for consistency
	if (nEvents > 0) {
	    if (verifyBackupDir() == 0) {
		state = STABLE_STATE;
	    }
	    else {
		makeBackupCopy();
	    }
	}
	else {
	    ibis::util::mutexLock lock(&ibis::util::envLock, backupDir);
	    ibis::util::removeDir(backupDir, true);
	    state = STABLE_STATE;
	}
    }
    else { // assumed to be in stable state
	state = STABLE_STATE;
    }

    if (nEvents > 0) { // read mask of the partition
	std::string mskfile(activeDir);
	if (! mskfile.empty())
	    mskfile += DIRSEP;
	mskfile += "-part.msk";
	try {
	    amask.read(mskfile.c_str());
	    if (amask.size() != nEvents) {
		if (ibis::gVerbose > 1 && amask.size() > 0)
		    ibis::util::logMessage
			("Warning", "mask file \"%s\" contains only %lu "
			 "bits but %lu was expected", mskfile.c_str(),
			 static_cast<long unsigned>(amask.size()),
			 static_cast<long unsigned>(nEvents));
		amask.adjustSize(nEvents, nEvents);
		if (amask.cnt() < nEvents)
		    amask.write(mskfile.c_str());
		else
		    remove(mskfile.c_str());
		ibis::fileManager::instance().flushFile(mskfile.c_str());
	    }
	}
	catch (...) {
	    if (ibis::gVerbose > 1)
		ibis::util::logMessage
		    ("Warning", "unable to read mask file \"%s\", assume "
		     "all %lu rows are active", mskfile.c_str(),
		     static_cast<long unsigned>(nEvents));
	    amask.set(1, nEvents);
	    remove(mskfile.c_str());
	}
    }

    // superficial checks
    j = 0;
    if (maxLength <= 0) maxLength = 16;
    if (strlen(activeDir)+16+maxLength > PATH_MAX) {
	ibis::util::logMessage
	    ("Warning", "directory name \"%s\" is too long",
	     activeDir);
	++j;
    }
    if (backupDir != 0 && strlen(backupDir)+16+maxLength > PATH_MAX) {
	ibis::util::logMessage
	    ("Warning", "directory name \"%s\" is too long",
	     backupDir);
	++j;
    }
    if (j) throw "direcotry names too long";

    myCleaner = new ibis::part::cleaner(this);
    ibis::fileManager::instance().addCleaner(myCleaner);

    if (ibis::gVerbose > 0 && m_name != 0) {
	ibis::util::logger lg(0);
	lg.buffer() << "Completed construction of an ";
	if (nEvents == 0)
	    lg.buffer() << "empty ";
	lg.buffer() << "ibis::part named "
		    << (m_name?m_name:"<NULL>") << ".\n";
	if (ibis::gVerbose > 1) {
	    lg.buffer() << "activeDir = \"" << activeDir << "\"\n";
	    if (backupDir != 0 && *backupDir != 0)
		lg.buffer() << "backupDir = \"" << backupDir << "\"\n";
	}
	if (columns.size() > 0) {
	    if (ibis::gVerbose > 3)
		print(lg.buffer());
	    else if (nEvents > 0)
		lg.buffer() << nEvents << " row"<< (nEvents>1 ? "s":"")
			    << ", " << columns.size() << " column"
			    << (columns.size()>1 ? "s" : "") << "\n";
	}
    }
} // ibis::part::init

// read the meta tag entry in the header section of the metadata file in
// directory dir
char* ibis::part::readMetaTags(const char* const dir) {
    char *s1;
    char* m_tags = 0;
    if (dir == 0)
	return m_tags;
    if (*dir == 0)
	return m_tags;

    char buf[MAX_LINE];
    long ierr = UnixSnprintf(buf, MAX_LINE, "%s%c-part.txt", dir, DIRSEP);
    if (ierr < 2 || ierr > MAX_LINE) {
	ibis::util::logMessage("Warning", "ibis::part::readMetaTags failed "
			       "to generate the metadata file name");
	return m_tags;
    }

    FILE* file = fopen(buf, "r");
    if (file == 0) {
	strcpy(buf+(ierr-9), "table.tdc"); // old metadata file name
	file = fopen(buf, "r");
    }
    if (file == 0) {
	if (ibis::gVerbose > 1)
	    ibis::util::logMessage("Warning",
				   "ibis::part::readMetaTags failed "
				   "to open file \"%s\" ... %s", buf,
				   (errno ? strerror(errno) :
				    "no free stdio stream"));
	return m_tags;
    }
    if (ibis::gVerbose > 4) {
	ibis::util::logMessage("ibis::part::readMetaTags()", "opened %s",
			       buf);
    }

    // skip till begin header
    while ((s1 = fgets(buf, MAX_LINE, file))) {
	if (strnicmp(buf, "BEGIN HEADER", 12) == 0) break;
    }

    // parse header -- read till end header
    while ((s1 = fgets(buf, MAX_LINE, file))) {
	if (strlen(buf) + 1 >= MAX_LINE) {
	    ibis::util::logMessage("Warning", "readMetaTags may have "
				   "encountered a line that has more than "
				   "%d characters.", MAX_LINE);
	}
	LOGGER(15) << buf;

	if (strnicmp(buf, "END HEADER", 10) == 0) {
	    break;
	}
	else if (strnicmp(buf, "metaTags", 8) == 0 ||
		 strnicmp(buf, "table.metaTags", 16) == 0 ||
		 strnicmp(buf, "DataSet.metaTags", 16) == 0) {
	    s1 = strchr(buf, '=');
	    if (s1!=0) {
		if (s1[1] != 0) {
		    ++ s1;
		    m_tags = ibis::util::getString(s1);
		}
	    }
	    break;
	}
	else if (strnicmp(buf, "Event_Set.metaTags", 18) == 0) {
	    s1 = strchr(buf, '=');
	    if (s1!=0) {
		if (s1[1] != 0) {
		    ++ s1;
		    m_tags = ibis::util::getString(s1);
		}
	    }
	    break;
	}
    } // the loop to parse header

    fclose(file); // close the tdc file

    return m_tags;
} // ibis::part::readMetaTags

// read the meta tag entry in the header section of the metadata file in
// directory dir
void ibis::part::readMeshShape(const char* const dir) {
    char *s1;
    if (dir == 0)
	return;
    if (*dir == 0)
	return;

    char buf[MAX_LINE];
    long ierr = UnixSnprintf(buf, MAX_LINE, "%s%c-part.txt", dir, DIRSEP);
    if (ierr < 2 || ierr > MAX_LINE) {
	ibis::util::logMessage("Warning", "ibis::part::readMeshShape "
			       "failed to generate the metadata file name");
	return;
    }

    FILE* file = fopen(buf, "r");
    if (file == 0) {
	strcpy(buf+(ierr-9), "table.tdc"); // old metadata file name
	file = fopen(buf, "r");
    }
    if (file == 0) {
	ibis::util::logMessage("Warning", "ibis::part::readMeshShape "
			       "failed to open file \"%s\" ... %s", buf,
			       (errno ? strerror(errno) :
				"no free stdio stream"));
	return;
    }
    if (ibis::gVerbose > 4) {
	ibis::util::logMessage("ibis::part::readMeshShape()",
			       "opened %s", buf);
    }

    // skip till begin header
    while ((s1 = fgets(buf, MAX_LINE, file))) {
	if (strnicmp(buf, "BEGIN HEADER", 12) == 0) break;
    }

    // parse header -- read till end header
    while ((s1 = fgets(buf, MAX_LINE, file))) {
	if (strlen(buf) + 1 >= MAX_LINE) {
	    ibis::util::logMessage("Warning", "readMeshShape may have "
				   "encountered a line that has more than "
				   "%d characters.", MAX_LINE);
	}
	LOGGER(15) << buf;

	if (strnicmp(buf, "END HEADER", 10) == 0) {
	    break;
	}
	else if (strnicmp(buf, "columnShape", 11) == 0 ||
		 strnicmp(buf, "Part.columnShape", 16) == 0 ||
		 strnicmp(buf, "Table.columnShape", 17) == 0 ||
		 strnicmp(buf, "DataSet.columnShape", 19) == 0 ||
		 strnicmp(buf, "Partition.columnShape", 21) == 0 ||
		 strnicmp(buf, "meshShape", 9) == 0 ||
		 strnicmp(buf, "Part.meshShape", 14) == 0 ||
		 strnicmp(buf, "Partition.meshShape", 19) == 0) {
	    s1 = strchr(buf, '(');
	    if (s1 != 0) {
		if (s1[1] != 0) {
		    ++ s1;
		    digestMeshShape(s1);
		}
	    }
	    break;
	}
    } // the loop to parse header

    fclose(file); // close the tdc file
} // ibis::part::readMeshShape

/// Read the metadata file from the named dir.  If dir is the activeDir,
/// it will also update the content of *this, otherwise it will only modify
/// arguments @c nrows and @c plist.  If this function completes
/// successfully, it returns the maximum length of the column names.
/// Otherwise, it returns a value of zero or less to indicate errors.
/// @remarks The metadata file is named "-part.txt".  It is a plain text
/// file with a fixed structure.  The prefix '-' is to ensure that none of
/// the data files could possibly have the same name (since '-' cann't
/// appear in any column names).  This file was previously named
/// "table.tdc" and this function still recognize this old name.
int ibis::part::readTDC(size_t& nrows, columnList& plist, const char* dir) {
    if (dir == 0 || *dir == 0) return -90;
    // clear the content of plist
    for (columnList::iterator it = plist.begin(); it != plist.end(); ++it)
	delete (*it).second;
    plist.clear();
    nrows = 0;

    std::string tdcname = dir;
    tdcname += DIRSEP;
    tdcname += "-part.txt";
    FILE* file = fopen(tdcname.c_str(), "r");
    if (file == 0) {
	tdcname.erase(tdcname.size()-9);
	tdcname += "table.tdc"; // the old name
	file = fopen(tdcname.c_str(), "r");
    }
    if (file == 0) {
	if (ibis::gVerbose > 7)
	    ibis::util::logMessage("ibis::part::readTDC", "failed to "
				   "open file \"%s\" ... %s", tdcname.c_str(),
				   (errno ? strerror(errno) :
				    "no free stdio stream"));
	return -91;
    }
    if (ibis::gVerbose > 4) {
	ibis::util::logMessage("ibis::part::readTDC", "opened %s",
			       tdcname.c_str());
    }

    char *s1;
    int  maxLength = 0;
    int  tot_columns = INT_MAX;
    int  num_columns = INT_MAX;
    bool isActive = (activeDir ? strcmp(activeDir, dir) == 0 : false);
    std::set<int> selected; // list of selected columns
    char buf[MAX_LINE];

    // skip till begin header
    while ((s1 = fgets(buf, MAX_LINE, file))) {
	if (strnicmp(buf, "BEGIN HEADER", 12) == 0) break;
    }

    // parse header -- read till end header
    while ((s1 = fgets(buf, MAX_LINE, file))) {
	if (strlen(buf) + 1 >= MAX_LINE) {
	    ibis::util::logMessage("Warning", "readTDC may have encountered "
				   "a line that has more than %d characters",
				   MAX_LINE);
	}
	LOGGER(7) << buf;

	// s1 points to the value come after = sign
	s1 = strchr(buf, '=');
	if (s1!=0) {
	    if (s1[1]!=0) {
		++ s1;
	    }
	    else {
		s1 = 0;
	    }
	}
	else {
	    s1 = 0;
	}

	if (strnicmp(buf, "END HEADER", 10) == 0) {
	    break;
	}
	else if (strnicmp(buf, "Number_of_rows", 14) == 0 ||
		 strnicmp(buf, "Number_of_events", 16) == 0 ||
		 strnicmp(buf, "Number_of_records", 17) == 0) {
	    nrows = atol(s1);
	    if (isActive)
		nEvents = nrows;
	}
	else if (strnicmp(buf, "Number_of_columns", 17) == 0 ||
		 strnicmp(buf, "Number_of_properties", 20) == 0) {
	    num_columns = atoi(s1);
	}
	else if (strnicmp(buf, "Tot_num_of", 10) == 0) {
	    tot_columns = atoi(s1);
	}
	else if (strnicmp(buf, "index", 5) == 0) {
	    delete [] idxstr; // discard the old value
	    idxstr = ibis::util::getString(s1);
#if defined(INDEX_SPEC_TO_LOWER)
	    s1 = idxstr + strlen(idxstr) - 1;
	    while (s1 >= idxstr) {
		*s1 = tolower(*s1);
		-- s1;
	    }
#endif
	}
	else if (strnicmp(buf, "Bins:", 5) == 0) {
	    delete [] idxstr; // discard teh old value
	    s1 = buf + 5;
	    idxstr = ibis::util::getString(s1);
#if defined(INDEX_SPEC_TO_LOWER)
	    s1 = idxstr + strlen(idxstr) - 1;
	    while (s1 >= idxstr) {
		*s1 = tolower(*s1);
		-- s1;
	    }
#endif
	}
	else if (strnicmp(buf, "Columns_Selected", 16) == 0 ||
		 strnicmp(buf, "Properties_Selected", 19) == 0) {
	    // the list can contain a list of ranges or numbers separated by
	    // ',', ';', or space
	    while (*s1 == 0) {
		char* s2;
		int i = atoi(s1);
		if (i > 0) {
		    selected.insert(i);
		}
		s2 = strchr(s1, '-');
		if (s2 != 0) {
		    s1 = s2 + 1;
		    int j = atoi(s1);
		    if (j < i) {
			ibis::util::logMessage
			    ("Warning", "readTDC encounters "
			     "an illformed range:\n%d%s", i, s2);
		    }
		    while (i<j) {
			++i;
			selected.insert(i);
		    }
		}
		s2 = strpbrk(s1, ",; \t");
		if (s2 != 0) {
		    s1 = s2 + 1;
		}
		else {
		    s1 = 0;
		}
	    }
	    if (num_columns == INT_MAX) {
		num_columns = selected.size();
	    }
	}
	else if (isActive) {
	    if ((strnicmp(buf, "Name", 4) == 0 &&
		 (isspace(buf[4]) || buf[4]=='=')) ||
		strnicmp(buf, "Table.Name", 10) == 0 ||
		strnicmp(buf, "DataSet.Name", 12) == 0 ||
		strnicmp(buf, "Partition.Name", 14) == 0 ||
		strnicmp(buf, "Part.Name", 9) == 0) {
		delete [] m_name; // discard the existing name
		m_name = ibis::util::getString(s1);
	    }
	    else if (strnicmp(buf, "Description", 11) == 0 ||
		     strnicmp(buf, "Table.Description", 17) == 0 ||
		     strnicmp(buf, "DataSet.Description", 19) == 0 ||
		     strnicmp(buf, "Partition.Description", 21) == 0 ||
		     strnicmp(buf, "Part.Description", 16) == 0) {
		delete [] m_desc;
		m_desc = ibis::util::getString(s1);
	    }
	    else if (strnicmp(buf, "metaTags", 8) == 0 ||
		     strnicmp(buf, "Table.metaTags", 14) == 0 ||
		     strnicmp(buf, "DataSet.metaTags", 16) == 0 ||
		     strnicmp(buf, "Partition.metaTags", 18) == 0 ||
		     strnicmp(buf, "Part.metaTags", 13) == 0) {
		ibis::resource::parseNameValuePairs(s1, metaList);
	    }
	    else if (strnicmp(buf, "Timestamp", 9) == 0) {
		if (sizeof(time_t) == sizeof(int))
		    switchTime = atoi(s1);
		else
		    switchTime = atol(s1);
	    }
	    else if (strnicmp(buf, "Alternative_Directory", 21) == 0) {
		if (activeDir == 0 || *activeDir == 0 ||
		    backupDir == 0 || *backupDir == 0 ||
		    (strcmp(s1, activeDir) != 0 &&
		     strcmp(s1, backupDir) != 0)) {
		    delete [] backupDir;
		    backupDir = ibis::util::getString(s1);
		}
	    }
	    else if (strnicmp(buf, "State", 5) == 0 ||
		     strnicmp(buf, "Part.State", 10) == 0 ||
		     strnicmp(buf, "Table.State", 11) == 0 ||
		     strnicmp(buf, "DataSet.State", 13) == 0 ||
		     strnicmp(buf, "Partition.State", 15) == 0) {
		state = (ibis::part::TABLE_STATE)atoi(s1);
	    }
	    else if (strnicmp(buf, "columnShape", 11) == 0 ||
		     strnicmp(buf, "Part.columnShape", 16) == 0 ||
		     strnicmp(buf, "Table.columnShape", 17) == 0 ||
		     strnicmp(buf, "DataSet.columnShape", 19) == 0 ||
		     strnicmp(buf, "Partition.columnShape", 21) == 0 ||
		     strnicmp(buf, "meshShape", 9) == 0 ||
		     strnicmp(buf, "Part.meshShape", 14) == 0 ||
		     strnicmp(buf, "Partition.meshShape", 19) == 0) {
		digestMeshShape(s1);
	    }
	}
    } // the loop to parse header

    // some minimal integrity check
    if ((uint32_t)num_columns != selected.size() && selected.size() > 0U) {
	ibis::util::logMessage
	    ("Warning", "Properties_Positions_Selected field "
	     "contains %lu elements,\nbut Number_of_columns "
	     "field is %lu", static_cast<long unsigned>(selected.size()),
	     static_cast<long unsigned>(num_columns));
	num_columns = selected.size();
    }
    if (tot_columns != INT_MAX &&
	tot_columns < num_columns) {
	ibis::util::logMessage
	    ("Warning", "Tot_num_of_prop (%lu) is less than "
	     "Number_of_columns(%lu", static_cast<long unsigned>(tot_columns),
	     static_cast<long unsigned>(num_columns));
	tot_columns = INT_MAX;
    }

    // start to parse columns
    int len, cnt=0;
    while ((s1 = fgets(buf, MAX_LINE, file))) {
	// get to the next "Begin Column" line
	if (strlen(buf) + 1 >= MAX_LINE) {
	    ibis::util::logMessage("Warning", "readTDC may have encountered "
				   "a line with more than %d characters.",
				   MAX_LINE);
	}

	if (strnicmp(buf, "Begin Column", 12) == 0 ||
	    strnicmp(buf, "Begin Property", 14) == 0) {
	    ++ cnt;
	    column* prop = new column(this, file);
	    if (ibis::gVerbose > 5)
		logMessage("readTDC", "got column %s from %s", prop->name(),
			   tdcname.c_str());

	    if (prop->type() == ibis::CATEGORY) {
		column* tmp = new ibis::category(*prop);
		delete prop;
		prop = tmp;
	    }
	    else if (prop->type() == ibis::TEXT) {
		column* tmp = new ibis::text(*prop);
		delete prop;
		prop = tmp;
	    }
	    if (selected.empty()) {
		// if Properties_Selected is not explicitly
		// specified, assume every column is to be included
		plist[prop->name()] = prop;
		len = strlen(prop->name());
		if (len > maxLength) maxLength = len;
	    }
	    else if (selected.find(cnt) != selected.end()) {
		// Properties_Positions_Selected is explicitly specified
		plist[prop->name()] = prop;
		len = strlen(prop->name());
		if (len > maxLength) maxLength = len;
	    }
	    else {
		// column is not selected
		delete prop;
	    }
	}
    } // parse columns
    fclose(file); // close the tdc file

    if ((uint32_t)num_columns != plist.size() && num_columns < INT_MAX) {
	ibis::util::logMessage("Warning", "ibis::part::readTDC found %lu "
			       "columns, but %lu were expected",
			       static_cast<long unsigned>(plist.size()),
			       static_cast<long unsigned>(num_columns));
    }
    if (cnt != tot_columns && tot_columns != INT_MAX) {
	ibis::util::logMessage("Warning", "ibis::part::readTDC expects %lu "
			       "columns, in the TDC file, but only %lu "
			       "entries were found",
			       static_cast<long unsigned>(tot_columns),
			       static_cast<long unsigned>(cnt));
    }

    if (isActive) {
	uint32_t mt = 0;
	// deal with the meta tags
	for (ibis::resource::vList::const_iterator mit = metaList.begin();
	     mit != metaList.end();
	     ++ mit) {
	    columnList::const_iterator cit = plist.find((*mit).first);
	    if (cit == plist.end()) { // need to add a new column
		ibis::category* prop =
		    new ibis::category(this, (*mit).first, (*mit).second,
				       dir, nrows);
		plist[prop->name()] = prop;
		++ mt;
	    }
	}
	// try to assign the directory name as the part name
	if (m_name == 0 || *m_name == 0) {
	    const char* cur = dir;
	    const char* lst = dir;
	    while (*cur != 0) {
		if (*cur == DIRSEP)
		    lst = cur;
		++ cur;
	    }
	    ++ lst;
	    if (lst < cur) { // found a directory name to copy
		delete [] m_name;
		m_name = ibis::util::strnewdup(lst);
		if (*cur != 0) {
		    // the incoming dir ended with DIRSEP, need to change
		    // it to null
		    len = cur - lst;
		    m_name[len-1] = 0;
		}
		++ mt;
	    }
	}

	if (mt > 0 && activeDir != 0 && *activeDir != 0) {
	    // write an updated TDC file
	    if (ibis::gVerbose > 1)
		logMessage("readTDC", "found %lu meta tags not recorded as "
			   "columns, writing new TDC file to %s",
			   static_cast<unsigned long>(mt), dir);
	    writeTDC(nEvents, plist, activeDir);
	    if (backupDir)
		writeTDC(nEvents, plist, activeDir);
	}
    }
    return maxLength;
} // ibis::part::readTDC

/// Write the metadata about the data partition into an ASCII file
/// "-part.txt".
void ibis::part::writeTDC(const uint32_t nrows, const columnList& plist,
			  const char* dir) const {
    if (dir == 0 || *dir == 0)
	return;
    char* filename = new char[strlen(dir)+16];
    sprintf(filename, "%s%c-part.txt", dir, DIRSEP);
    FILE *fptr = fopen(filename, "w");
    if (fptr == 0) {
	ibis::util::logMessage
	    ("Warning", "writeTDC failed to open file \"%s\" for writing "
	     "... %s", filename, (errno ? strerror(errno) :
				  "no free stdio stream"));
	delete [] filename;
	return;
    }

    bool isActive = (activeDir != 0 ? (strcmp(activeDir, dir) == 0) : false);
    bool isBackup = (backupDir != 0 ? (strcmp(backupDir, dir) == 0) : false);
    char stamp[28];
    ibis::util::getGMTime(stamp);
    fprintf(fptr, "# metadata file written by ibis::part::writeTDC\n"
	    "# on %s UTC\n\n", stamp);
    if (m_name != 0) {
	fprintf(fptr, "BEGIN HEADER\nName = \"%s\"\n", m_name);
    }
    else { // make up a name based on the time stamp
	std::string nm;
	uint32_t tmp = ibis::util::checksum(stamp, strlen(stamp));
	ibis::util::int2string(nm, tmp);
	if (! isalpha(nm[0]))
	    nm[0] = 'A' + (nm[0] % 26);
	fprintf(fptr, "BEGIN HEADER\nName = \"%s\"\n", nm.c_str());
    }
    if (m_desc != 0 && (isActive || isBackup)) {
	fprintf(fptr, "Description = \"%s\"\n", m_desc);
    }
    else {
	fprintf(fptr, "Description = \"This table was created "
		"on %s UTC with %lu rows and %lu columns.\"\n",
		stamp, static_cast<long unsigned>(nrows),
		static_cast<long unsigned>(plist.size()));
    }
    if (! metaList.empty()) {
	std::string mt = metaTags();
	fprintf(fptr, "Table.metaTags = %s\n", mt.c_str());
    }

    fprintf(fptr, "Number_of_columns = %lu\n",
	    static_cast<long unsigned>(plist.size()));
    fprintf(fptr, "Number_of_rows = %lu\n",
	    static_cast<long unsigned>(nrows));
    if (shapeSize.size() > 0) {
	fprintf(fptr, "columnShape = (");
	for (uint32_t i = 0; i < shapeSize.size(); ++i) {
	    if (i > 0)
		fprintf(fptr, ", ");
	    if (shapeName.size() > i && ! shapeName[i].empty())
		fprintf(fptr, "%s=%lu", shapeName[i].c_str(),
			static_cast<long unsigned>(shapeSize[i]));
	    else
		fprintf(fptr, "%lu",
			static_cast<long unsigned>(shapeSize[i]));
	    fprintf(fptr, ")\n");
	}
    }
    if (isActive) {
	if (backupDir != 0 && *backupDir != 0 && backupDir != activeDir &&
	    strcmp(activeDir, backupDir) != 0)
	    fprintf(fptr, "Alternative_Directory = \"%s\"\n", backupDir);
    }
    else if (isBackup) {
	fprintf(fptr, "Alternative_Directory = \"%s\"\n", activeDir);
    }
    if (isActive || isBackup) {
	fprintf(fptr, "Timestamp = %lu\n",
		static_cast<long unsigned>(switchTime));
	fprintf(fptr, "State = %d\n", (int)state);
    }
    if (idxstr != 0) {
	fprintf(fptr, "index = %s\n", idxstr);
    }
    fputs("END HEADER\n", fptr);

    for (columnList::const_iterator it = plist.begin();
	 it != plist.end(); ++ it)
	(*it).second->write(fptr);
    fclose(fptr);
    if (ibis::gVerbose > 4)
	logMessage("writeTDC", "wrote a new table file for %lu rows and %lu "
		   "columns to \"%s\"",
		   static_cast<long unsigned>(nrows),
		   static_cast<long unsigned>(plist.size()), filename);
    delete [] filename;
} // ibis::part::writeTDC()

/// Make a deep copy of the incoming name-value pairs.
void ibis::part::setMetaTags(const ibis::resource::vList& mts) {
    for (ibis::resource::vList::const_iterator it = mts.begin();
	 it != mts.end();
	 ++ it)
	metaList[ibis::util::strnewdup((*it).first)] =
	    ibis::util::strnewdup((*it).second);
} // ibis::part::setMetaTags

/// Make a deep copy of the incoming name-value pairs.  The even element is
/// assumed to be the name and the odd element is assumed to be the value.
/// If the last name is not followed by a value it is assumed to have the
/// value of '*', which indicates do-not-care.
void ibis::part::setMetaTags(const std::vector<const char*>& mts) {
    ibis::resource::clear(metaList); // clear the current content
    const uint32_t len = mts.size();
    for (uint32_t i = 0; i+1 < len; i += 2) {
	metaList[ibis::util::strnewdup(mts[i])] =
	    ibis::util::strnewdup(mts[i+1]);
    }
    if (len % 2 == 1) { // add name with a do-not-care value
	metaList[ibis::util::strnewdup(mts.back())] =
	    ibis::util::strnewdup("*");
    }
} // ibis::part::setMetaTags()

/// Output meta tags as a string.
std::string ibis::part::metaTags() const {
    std::string st;
    st.erase();
    for (ibis::resource::vList::const_iterator it = metaList.begin();
	 it != metaList.end();
	 ++ it) {
	if (! st.empty())
	    st += ", ";
	st += (*it).first;
	st += " = ";
	st += (*it).second;
    }
    return st;
}

// return true if the list of meta tags contains a name-value pair that
// matches the input arguments
bool ibis::part::matchNameValuePair(const char* name, const char* value)
    const {
    bool ret = false;
    if (name == 0) return ret;
    if (*name == 0) return ret;
    ibis::resource::vList::const_iterator it = metaList.find(name);
    if (it != metaList.end()) {
	if (value == 0) {
	    ret = true;
	}
	else if (*value == 0) {
	    ret = true;
	}
	else if ((value[0] == '*' && value[1] == 0) ||
		 ((*it).second[0] == '*' || (*it).second[0] == 0)) {
	    ret = true;
	}
	else {
	    ret = ibis::util::strMatch((*it).second, value);
	}
    }
    return ret;
} // ibis::part::matchNameValuePair

bool
ibis::part::matchMetaTags(const std::vector<const char*>& mtags) const {
    bool ret = true;
    const uint32_t len = mtags.size();
    for (uint32_t i = 0; ret && (i+1 < len); i += 2) {
	ret = matchNameValuePair(mtags[i], mtags[i+1]);
    }
    return ret;
} // ibis::part::matchMetaTags

// the two vLists must match exactly
bool ibis::part::matchMetaTags(const ibis::resource::vList& mtags) const {
    const uint32_t len = mtags.size();
    bool ret = (metaList.size() == mtags.size());
    ibis::resource::vList::const_iterator it1 = mtags.begin();
    ibis::resource::vList::const_iterator it2 = metaList.begin();
    for (uint32_t i = 0; ret && (i < len); ++i, ++it1, ++it2) {
	ret = ((stricmp((*it1).first, (*it2).first) == 0) &&
	       ((strcmp((*it1).second, "*")==0) ||
		(strcmp((*it2).second, "*")==0) ||
		(stricmp((*it1).second, (*it2).second)==0)));
	if (ibis::gVerbose > 5) {
	    if (ret)
		logMessage("matchMetaTags", "meta tags (%s = %s) and "
			   "(%s = %s) match", (*it1).first, (*it1).second,
			   (*it2).first, (*it2).second);
	    else
		logMessage("matchMetaTags", "meta tags (%s = %s) and "
			   "(%s = %s) donot match",
			   (*it1).first, (*it1).second,
			   (*it2).first, (*it2).second);
	}
    }
    return ret;
} // ibis::part::matchMetaTags

// digest the columne shape string read from TDC file
void ibis::part::digestMeshShape(const char *shape) {
    if (shape == 0) return;
    while (*shape && isspace(*shape))
	++ shape;
    if (*shape == 0) return;

    // clear the current shape
    shapeSize.clear();
    shapeName.clear();

    const char* str = shape + strspn(shape, " \t(");
    while (str) {
	const char* tmp;
	std::string dname;
	if (isalpha(*str)) { // collect the name
	    tmp = strchr(str, '=');
	    while (str < tmp) { // copy every byte except space
		if (! isspace(*str))
		    dname += *str;
		++ str;
	    }
	    str = tmp + strspn(tmp, " \t=");
	}
	while (*str && !isdigit(*str)) // skip everything not a digit
	    ++ str;

	uint32_t dim = 0;
	if (*str)
	    dim = atol(str);
	if (dim > 0) { // record the name and value pair
	    shapeSize.push_back(dim);
	    shapeName.push_back(dname);
	}
	if (*str) {
	    tmp = strpbrk(str, " \t,;");
	    if (tmp)
		str = tmp + strspn(tmp, " \t,;");
	    else
		str = 0;
	}
	else {
	    str = 0;
	}
    }

    if (ibis::gVerbose > 6) {
	std::ostringstream ostr;
	for (uint32_t i = 0; i < shapeSize.size(); ++i) {
	    if (i > 0)
		ostr << ", ";
	    if (shapeName.size() > i && ! shapeName[i].empty())
		ostr << shapeName[i] << "=";
	    ostr << shapeSize[i];
	}
	logMessage("digestMeshShape", "converted string \"%s\" to "
		   "shape (%s)", shape, ostr.str().c_str());
    }
} // ibis::part::digestMeshShape

void ibis::part::combineNames(ibis::table::namesTypes& metalist) const {
    for (ibis::part::columnList::const_iterator cit = columns.begin();
	 cit != columns.end(); ++ cit) {
	const ibis::column* col = (*cit).second;
	ibis::table::namesTypes::const_iterator nit =
	    metalist.find((*cit).first);
	if (nit == metalist.end()) { // a new column
	    metalist[col->name()] = col->type();
	}
	else if ((*nit).second != col->type()) { // type must match
	    logWarning("combineNames", "column %s is of type \"%s\", "
		       "but it is type \"%s\" in the combined list",
		       col->name(), ibis::TYPESTRING[(int)col->type()],
		       ibis::TYPESTRING[(int)(*nit).second]);
	}
    }
} // ibis::part::combineNames

void ibis::part::print(std::ostream& out) const {
    //    readLock lock(this, "print");
    if (m_name == 0) return;
    out << "ibis::part: " << m_name << "\n";
    if (m_desc != 0) out << m_desc << "\n";
    if (rids != 0 && rids->size() > 0)
	out << "There are " << rids->size() << " RIDs/rows, "
	    << columns.size() << " columns" << std::endl;
    else
	out << "There are " << nEvents << " RIDs/rows, "
	    << columns.size() << " columns" << std::endl;
    if (columns.size() > 0) {
	out << "\nColumn list:\n";
	if (colorder.empty()) {
	    for (columnList::const_iterator it = columns.begin();
		 it != columns.end(); ++it) {
		out << *((*it).second) << "\n";
	    }
	}
	else if (colorder.size() == columns.size()) {
	    for (size_t i = 0; i < columns.size(); ++ i)
		out << colorder[i]->name() << "\n";
	}
	else {
	    std::set<const char*, ibis::lessi> names;
	    for (ibis::part::columnList::const_iterator it = columns.begin();
		 it != columns.end(); ++ it)
		names.insert((*it).first);
	    for (size_t i = 0; i < colorder.size(); ++ i) {
		out << colorder[i]->name() << "\n";
		names.erase(colorder[i]->name());
	    }
	    for (std::set<const char*, ibis::lessi>::const_iterator it =
		     names.begin(); it != names.end(); ++ it)
		out << *it << "\n";
	}
	out << std::endl;
    }
} // ibis::part::print

// a function to retrieve RIDs stored in file
void ibis::part::readRIDs() const {
    if (activeDir == 0) return;

    readLock lock(this, "readRIDs");
    if (rids) {
	if (rids->size() == nEvents)
	    return;
	else
	    delete rids;
    }

    std::string fn(activeDir);
    fn += DIRSEP;
    fn += "rids";
    rids = new array_t<ibis::rid_t>;
    if (ibis::fileManager::instance().getFile(fn.c_str(), *rids)) {
	if (ibis::gVerbose > 3)
	    ibis::util::logMessage("ibis::part::readRIDs", "the file "
				   "manager failed to read file "
				   "\"%s\".  There is no RIDs.",
				   fn.c_str());
	rids->clear();
    }
    if (nEvents != rids->size() && rids->size() > 0) {
	if (ibis::gVerbose > 2) {
	    ibis::util::logMessage("Warning",  "nEvents (%lu) is different "
				   "from the number of RIDs (%lu).",
				   static_cast<long unsigned>(nEvents),
				   static_cast<long unsigned>(rids->size()));
	}
    }
} // ibis::part::readRIDs

// a function to remove RIDs
void ibis::part::freeRIDs() const {
    if (rids) {
	advisoryLock lock(this, "freeRIDs");
	if (lock.acquired()) {
	    // only perform deletion if it actually acquired a write lock
	    delete rids;
	    rids = 0;
	}
    }
} // ibis::part::freeRIDs

// generate arbitrary RIDs to that we can functioin correctly
void ibis::part::fillRIDs(const char* fn) const {
    if (nEvents == 0) return; // can not do anything

    FILE* rf = fopen(fn, "wb");
    std::string sfile(fn);
    sfile += ".srt";
    FILE* sf = fopen(sfile.c_str(), "wb");

    uint32_t ir = ibis::fileManager::instance().iBeat();
    rid_t tmp;
    tmp.num.run = ir;
    tmp.num.event = 0;
    rids->resize(nEvents);
    if (rf && sf) {
	for (uint32_t i = 0; i < nEvents; ++i) {
	    ++ tmp.value;
	    (*rids)[i].value = tmp.value;
	    fwrite(&((*rids)[i].value), sizeof(rid_t), 1, rf);
	    fwrite(&((*rids)[i].value), sizeof(rid_t), 1, sf);
	    fwrite(&i, sizeof(uint32_t), 1, sf);
	}
	fclose(rf);
	fclose(sf);
    }
    else {
	if (rf) fclose(rf);
	if (sf) fclose(sf);
	for (uint32_t i = 0; i < nEvents; ++i) {
	    ++ tmp.value;
	    (*rids)[i].value = tmp.value;
	}
    }
} // ibis::part::fillRIDs

// generate a sorted version of the RIDs and stored the result in rids.srt
// NOTE on locking -- it should be run only once.
// since one of the caller to this function may hold a read lock on the
// ibis::part object, it will cause a dead lock if this function also
// attempt to acquire the mutex lock on the partition.
void ibis::part::sortRIDs() const {
    if (activeDir == 0) return;

    char name[PATH_MAX];
    mutexLock lck(this, "sortRIDs");
    //ibis::util::mutexLock lck(&ibis::util::envLock, "sortRIDs");
    sprintf(name, "%s%crids.srt", activeDir, DIRSEP);
    uint32_t sz = ibis::util::getFileSize(name);
    if (sz == nEvents*(sizeof(rid_t)+sizeof(uint32_t)))
	return;
    if (sz > 0) {
	// remove it from the file cache
	ibis::fileManager::instance().flushFile(name);
	remove(name); // remove the existing (wrong-sized) file
    }

    typedef std::map< const ibis::rid_t*, uint32_t,
	std::less<const ibis::rid_t*> > RIDmap;
    RIDmap rmap;
    ibis::horometer timer;
    timer.start();
    // insert all rids into the RIDmap
    for (uint32_t i=0; i<nEvents; ++i)
	rmap[&(*rids)[i]] = i;
    if (rids->size() != rmap.size()) {
	logWarning("sortRIDs",
		   "There are %lu unique RIDs out of %lu total RIDs",
		   static_cast<long unsigned>(rmap.size()),
		   static_cast<long unsigned>(rids->size()));
    }

    // write the sorted rids into rids.srt
    uint32_t buf[2];
    int fdes = UnixOpen(name, OPEN_WRITEONLY, OPEN_FILEMODE);
    if (fdes < 0) {
	logWarning("sortRIDs", "failed to open file %s for writing ... %s",
		   name, (errno ? strerror(errno) : "no free stdio stream"));
	return;
    }
#if defined(_WIN32) && defined(_MSC_VER)
    (void)_setmode(fdes, _O_BINARY);
#endif

    const uint32_t nbuf = sizeof(buf);
    for (RIDmap::const_iterator it = rmap.begin(); it != rmap.end(); ++it) {
	buf[0] = (*it).first->num.run;
	buf[1] = (*it).first->num.event;
	int32_t ierr = UnixWrite(fdes, buf, nbuf);
	ierr += UnixWrite(fdes, &((*it).second), sizeof(uint32_t));
	if (ierr <= 0 ||
	    static_cast<uint32_t>(ierr) != nbuf+sizeof(uint32_t)) {
	    logWarning("sortRIDs", "failed to write run (%lu, %lu, %lu) to "
		       "file %s", static_cast<long unsigned>(buf[0]),
		       static_cast<long unsigned>(buf[1]),
		       static_cast<long unsigned>((*it).second), name);
	    UnixClose(fdes);
	    remove(name);
	    return;
	}
    }
    UnixClose(fdes);
    timer.stop();
    if (ibis::gVerbose > 4)
	logMessage("sortRIDs", "sorting %lu RIDs took  %g sec(CPU) %g "
		   "sec(elapsed); result written to %s",
		   static_cast<long unsigned>(rmap.size()),
		   timer.CPUTime(), timer.realTime(), name);
} // ibis::part::sortRIDs

// It tries the sorted RID list first.  If that fails, it uses the brute
// force searching algorithm
uint32_t ibis::part::getRowNumber(const ibis::rid_t& rid) const {
    uint32_t ind = searchSortedRIDs(rid);
    if (ind >= nEvents)
	ind = searchRIDs(rid);
    return ind;
} // ibis::part::getRowNumber

// use file rids.srt to search for the rid
uint32_t ibis::part::searchSortedRIDs(const ibis::rid_t& rid) const {
    uint32_t ind = nEvents;
    if (activeDir == 0) return ind;

    char name[PATH_MAX];
    // NOTE
    // really should not use the following lock, however, it appears that
    // g++ 2.95.2 is not generating the correct code to deallocate ridx at
    // the end of the function call.  This mutex lock helps to resolve the
    // problem.
    //mutexLock lock(this, "searchSortedRIDs");
    sprintf(name, "%s%crids.srt", activeDir, DIRSEP);
    array_t<uint32_t> ridx;
    int ierr = ibis::fileManager::instance().getFile(name, ridx);
    if (ierr != 0) {
	sortRIDs(); // generate rids.srt file from rids
	ierr = ibis::fileManager::instance().getFile(name, ridx);
	if (ierr != 0) {
	    logWarning("searchSortedRIDs",
		       "unable to generate rids.srt (%s)",
		       name);
	    return ind;
	}
    }
    if (ridx.size() < 3)
	return ind;

    // binary search
    uint32_t lower = 0, upper = ridx.size() / 3;
    while (lower < upper) {
	ind = (lower + upper) / 2;
	uint32_t ind3 = ind*3;
	if (rid.num.run < ridx[ind3]) {
	    upper = ind;
	}
	else if (rid.num.run > ridx[ind3]) {
	    if (ind == lower) break;
	    lower = ind;
	}
	else if (rid.num.event < ridx[ind3+1]) {
	    upper = ind;
	}
	else if (rid.num.event > ridx[ind3+1]) {
	    if (ind == lower) break;
	    lower = ind;
	}
	else {
	    ind = ridx[ind3+2];
#if defined(DEBUG) && DEBUG + 0 > 1
	    logMessage("searchSortedRIDs", "RID(%lu, %lu) ==> %lu",
		       static_cast<long unsigned>(rid.num.run),
		       static_cast<long unsigned>(rid.num.event),
		       static_cast<long unsigned>(ind));
#endif
	    return ind;
	}
    }

#if defined(DEBUG) && DEBUG + 0 > 1
    logMessage("searchSortedRIDs", "can not find RID(%lu, %lu)",
	       static_cast<long unsigned>(rid.num.run),
	       static_cast<long unsigned>(rid.num.event));
#endif
    ind = nEvents;
    return ind;
} // ibis::part::searchSortedRIDs

// brute-force search on rids
uint32_t ibis::part::searchRIDs(const ibis::rid_t& rid) const {
    uint32_t i = nEvents;
    for (i=0; i<nEvents; ++i) {
	if ((*rids)[i].value == rid.value) {
#if defined(DEBUG) && DEBUG + 0 > 1
	    logMessage("searchRIDs", "RID(%lu, %lu) ==> %lu",
		       static_cast<long unsigned>(rid.num.run),
		       static_cast<long unsigned>(rid.num.event),
		       static_cast<long unsigned>(i));
#endif
	    return i;
	}
    }
#if defined(DEBUG) && DEBUG + 0 > 1
    logMessage("searchRIDs", "can not find RID(%lu, %lu)",
	       static_cast<long unsigned>(rid.num.run),
	       static_cast<long unsigned>(rid.num.event));
#endif
    return i;
} // ibis::part::searchRIDs

// use file rids.srt to search for the rid
// assume the incoming RIDs are sorted
void ibis::part::searchSortedRIDs(const ibis::RIDSet& in,
				  ibis::bitvector& res) const {
    if (activeDir == 0) return;
    char name[PATH_MAX];
    sprintf(name, "%s%crids.srt", activeDir, DIRSEP);
    array_t<uint32_t> ridx;
    int ierr = ibis::fileManager::instance().getFile(name, ridx);
    if (ierr != 0) {
	sortRIDs(); // generate rids.srt file from rids
	ierr = ibis::fileManager::instance().getFile(name, ridx);
	if (ierr != 0) {
	    logWarning("searchSortedRIDs",
		       "unable to generate rids.srt (%s)",
		       name);
	    searchRIDs(in, res);
	    return;
	}
    }
    if (ridx.size() != 3*nEvents) {
	// Even though we have read the rids.srt file correctly, but the
	// file is not the right size.  Need a way to delete the references
	// to the file rids.srt.
	array_t<uint32_t> *tmp = new array_t<uint32_t>;
	tmp->swap(ridx);
	delete tmp;

	searchRIDs(in, res);
	return;
    }
    if (in.size() > 100) {
	res.set(0, nEvents);
	res.decompress();
    }
    else {
	res.clear();
    }

    // matching two sorted lists
    uint32_t i0 = 0, i1 = 0;
    while (i0 < 3*nEvents && i1 < in.size()) {
	if (in[i1].num.run > ridx[i0]) {
	    i0 += 3;
	}
	else if (in[i1].num.run < ridx[i0]) {
	    ++ i1;
	}
	else if (in[i1].num.event > ridx[i0+1]) {
	    i0 += 3;
	}
	else if (in[i1].num.event < ridx[i0+1]) {
	    ++ i1;
	}
	else { // two RIDs are the same
	    res.setBit(ridx[i0+2], 1);
#if defined(DEBUG) && DEBUG + 0 > 1
	    logMessage("searchSortedRIDs", "RID(%lu, %lu) ==> %lu",
		       static_cast<long unsigned>(ridx[i0]),
		       static_cast<long unsigned>(ridx[i0+1]),
		       static_cast<long unsigned>(ridx[i0+2]));
#endif
	    i0 += 3;
	    ++ i1;
	}
    }

    res.compress();
    res.adjustSize(0, nEvents);
} // ibis::part::searchSortedRIDs

// brute-force search on rids
// assume the incoming RIDs are sorted
void ibis::part::searchRIDs(const ibis::RIDSet& in,
			    ibis::bitvector& res) const {
    uint32_t i = nEvents, cnt = 0;
    if (in.size() > 100) {
	res.set(0, nEvents);
	res.decompress();
    }
    else
	res.clear();
    for (i=0; i<nEvents && cnt<in.size(); ++i) {
	ibis::RIDSet::const_iterator it = std::find(in.begin(), in.end(),
						    (*rids)[i]);
	if (it != in.end()) { // found one
#if defined(DEBUG) && DEBUG + 0 > 1
	    logMessage("searchRIDs", "RID(%lu, %lu) ==> %lu",
		       static_cast<long unsigned>((*it).num.run),
		       static_cast<long unsigned>((*it).num.event),
		       static_cast<long unsigned>(i));
#endif
	    res.setBit(i, 1);
	}
    }

    res.compress();
    res.adjustSize(0, nEvents);
} // ibis::part::searchRIDs

// retrieve only the RIDs corresponding to mask[i] == 1
array_t<ibis::rid_t>* ibis::part::getRIDs(const ibis::bitvector& mask)
    const {
    const uint32_t cnt = mask.cnt();
    array_t<ibis::rid_t>* ret = new array_t<ibis::rid_t>;
    if (cnt == 0)
	return ret;
    if (rids == 0)
	return ret;
    if (rids->size() != nEvents)
	return ret;

    ret->reserve(cnt);
    ibis::bitvector::indexSet ind = mask.firstIndexSet();
    if (rids == 0)
	readRIDs(); // attempt to read the file rids
    else if (rids->size() == 0)
	readRIDs();

    if (rids !=0 && rids->size() > 0) { // has rids specified
	readLock lock(this, "getRIDs");
	const uint32_t nmask = mask.size();
	const uint32_t nrids = rids->size();
	if (nrids != nmask)
	    logMessage("getRIDs", "number of RIDs (%lu) does not match the "
		       "size of the mask (%lu)",
		       static_cast<long unsigned>(nrids),
		       static_cast<long unsigned>(nmask));
	if (nrids >= nmask) { // more RIDS than mask bits
	    while (ind.nIndices()) {    // copy selected elements
		const uint32_t nind = ind.nIndices();
		const ibis::bitvector::word_t *idx = ind.indices();
		if (ind.isRange()) {
		    for (uint32_t j = *idx; j < idx[1]; ++j)
			ret->push_back((*rids)[j]);
		}
		else {
		    for (uint32_t j = 0; j < nind; ++j)
			ret->push_back((*rids)[idx[j]]);
		}
		++ ind;
	    }
	}
	else { // less RIDS than mask bits, need to check loop indices
	    while (ind.nIndices()) {    // copy selected elements
		const uint32_t nind = ind.nIndices();
		const ibis::bitvector::word_t *idx = ind.indices();
		if (*idx >= nrids) {
		    return ret;
		}
		if (ind.isRange()) {
		    for (uint32_t j = *idx;
			 j < (idx[1]<=nrids ? idx[1] : nrids);
			 ++j)
			ret->push_back((*rids)[j]);
		}
		else {
		    for (uint32_t j = 0; j < nind; ++j) {
			if (idx[j] < nrids)
			    ret->push_back((*rids)[idx[j]]);
			else
			    break;
		    }
		}
		++ ind;
	    }
	}
    }
    else { // use the row numbers as RIDs
	while (ind.nIndices()) {
	    const uint32_t nind = ind.nIndices();
	    const ibis::bitvector::word_t *idx = ind.indices();
	    rid_t tmp;
	    if (ind.isRange()) {
		for (uint32_t j = *idx; j < idx[1]; ++j) {
		    tmp.value = j;
		    ret->push_back(tmp);
		}
	    }
	    else {
		for (uint32_t j = 0; j < nind; ++j) {
		    tmp.value = idx[j];
		    ret->push_back(tmp);
		}
	    }
	    ++ ind;
	}
    }

    if (ret->size() != cnt) {
	logWarning("getRIDs", "expected to get %lu RIDs, but actually "
		   "got %lu", static_cast<long unsigned>(cnt),
		   static_cast<long unsigned>(ret->size()));
    }
    return ret;
} // ibis::part::getRIDs

uint32_t ibis::part::countPages(const ibis::bitvector& mask,
				unsigned wordsize) {
    uint32_t res = 0;
    if (mask.cnt() == 0)
	return res;
    if (wordsize == 0)
	return res;

    // words per page
    const uint32_t wpp = ibis::fileManager::pageSize() / wordsize;
    uint32_t last;  // the position of the last entry encountered
    ibis::bitvector::indexSet ix = mask.firstIndexSet();
    last = *(ix.indices());
    if (ibis::gVerbose < 8) {
	while (ix.nIndices() > 0) {
	    const ibis::bitvector::word_t *ind = ix.indices();
	    const uint32_t p0 = *ind / wpp;
	    res += (last < p0*wpp); // last not on the current page
	    if (ix.isRange()) {
		res += (ind[1] / wpp - p0);
		last = ind[1];
	    }
	    else {
		last = ind[ix.nIndices()-1];
		res += (last / wpp > p0);
	    }
	    ++ ix;
	}
    }
    else {
	ibis::util::logger lg(8);
	lg.buffer() << "ibis::part::countPages(" << wordsize
		    << ") page numbers: ";
	while (ix.nIndices() > 0) {
	    const ibis::bitvector::word_t *ind = ix.indices();
	    const uint32_t p0 = *ind / wpp;
	    if (last < p0*wpp) { // last not on the current page
		lg.buffer() << last/wpp << " ";
		++ res;
	    }
	    if (ix.isRange()) {
		const unsigned mp = ind[1]/wpp - p0;
		if (mp > 1)
		    lg.buffer() << p0 << "*" << mp << " ";
		else if (mp > 0)
		    lg.buffer() << p0 << " ";
		last = ind[1];
		res += mp;
	    }
	    else {
		last = ind[ix.nIndices()-1];
		if (last / wpp > p0) {
		    lg.buffer() << p0 << " ";
		    ++ res;
		}
	    }
	    ++ ix;
	}
    }
    return res;
} // ibis::part::countPages

ibis::fileManager::ACCESS_PREFERENCE
ibis::part::accessHint(const ibis::bitvector& mask,
		       unsigned elem) const {
    ibis::fileManager::ACCESS_PREFERENCE hint =
	ibis::fileManager::MMAP_LARGE_FILES;
    if (elem == 0 || mask.size() == 0)
	return hint;

    const uint32_t npages = static_cast<uint32_t>
	(ceil(static_cast<double>(nEvents) * elem
	      / ibis::fileManager::pageSize()));
    // too few values selected or too many values selected, use default
    if (mask.cnt() < (npages >> 4) || (mask.bytes() >> 5) > npages)
	return hint;

    // count the number of pages, get first and last page number
    const uint32_t wpp = ibis::fileManager::pageSize() / elem;
    uint32_t first; // first page number
    uint32_t last;  // the position of the last entry encountered
    uint32_t cnt = 0;
    ibis::bitvector::indexSet ix = mask.firstIndexSet();
    last = *(ix.indices());
    first = *(ix.indices()) / wpp;
    while (ix.nIndices() > 0) {
	const ibis::bitvector::word_t *ind = ix.indices();
	const uint32_t p0 = *ind / wpp;
	cnt += (last < p0*wpp); // last not on the current page
	if (ix.isRange()) {
	    cnt += (ind[1] / wpp - p0);
	    last = ind[1];
	}
	else {
	    last = ind[ix.nIndices()-1];
	    cnt += (last / wpp > p0);
	}
	++ ix;
    }
    last /= wpp; // the last page number
    if (2*(last-first) < npages) {
	// pages to be accessed are concentrated
	if (cnt > 24 && (cnt+cnt >= last-first ||
			 last-first <= (npages >> 3)))
	    hint = ibis::fileManager::PREFER_MMAP;
    }
    else if (cnt > (npages >> 4)) {
	// more than 1/16th of the pages will be accessed
	hint = ibis::fileManager::PREFER_READ;
    }
    if (ibis::gVerbose > 4)
	logMessage("accessHint", "nRows=%lu, selected=%lu, #pages=%lu, "
		   "first page=%lu, last page=%lu, hint=%s",
		   static_cast<long unsigned>(nRows()),
		   static_cast<long unsigned>(mask.cnt()),
		   static_cast<long unsigned>(cnt),
		   static_cast<long unsigned>(first),
		   static_cast<long unsigned>(last),
		   (hint == ibis::fileManager::MMAP_LARGE_FILES ?
		    "MMAP_LARGE_FILES" :
		    hint == ibis::fileManager::PREFER_READ ?
		    "PREFER_READ" : "PREFER_MMAP"));
    return hint;
} // ibis::part::accessHint

// retrieve value for the qualified events
array_t<int32_t>* ibis::part::selectInts
(const char* pname, const ibis::bitvector& mask) const {
    columnList::const_iterator it = columns.find(pname);
    if (it != columns.end()) { // got it
	return (*it).second->selectInts(mask);
    }
    else {
	return 0;
    }
} // ibis::part::selectInts

array_t<uint32_t>* ibis::part::selectUInts
(const char* pname, const ibis::bitvector& mask) const {
    columnList::const_iterator it = columns.find(pname);
    if (it != columns.end()) { // got it
	return (*it).second->selectUInts(mask);
    }
    else {
	return 0;
    }
} // ibis::part::selectUInts

array_t<int64_t>* ibis::part::selectLongs
(const char* pname, const ibis::bitvector& mask) const {
    columnList::const_iterator it = columns.find(pname);
    if (it != columns.end()) { // got it
	return (*it).second->selectLongs(mask);
    }
    else {
	return 0;
    }
} // ibis::part::selectLongs

array_t<float>* ibis::part::selectFloats
(const char* pname, const ibis::bitvector& mask) const {
    columnList::const_iterator it = columns.find(pname);
    if (it != columns.end()) { // got it
	return (*it).second->selectFloats(mask);
    }
    else {
	return 0;
    }
} // ibis::part::selectFloats

array_t<double>* ibis::part::selectDoubles
(const char* pname, const ibis::bitvector& mask) const {
    columnList::const_iterator it = columns.find(pname);
    if (it != columns.end()) { // got it
	return (*it).second->selectDoubles(mask);
    }
    else {
	return 0;
    }
} // ibis::part::selectDoubles

// convert a list of RIDs into a bitvector
long ibis::part::evaluateRIDSet(const ibis::RIDSet& in,
				ibis::bitvector& hits) const {
    if (in.empty() || nEvents == 0)
	return 0;
    if (rids && rids->size() > 0) {
	try {
	    sortRIDs(); // make sure the file rids.srt is up-to-date
	    searchSortedRIDs(in, hits);
	}
	catch (...) { // don't care about the exception
	    searchRIDs(in, hits); // use bruteforce search
	}
    }
    else {
	// take the values in ridset to be row indices
	for (uint32_t i = 0; i < in.size(); ++i)
	    hits.setBit(static_cast<unsigned>(in[i].value), 1);
	hits.adjustSize(0, nEvents); // size the bitvector hits
    }
    if (ibis::gVerbose > 4)
	logMessage("evaluateRIDSet", "found %lu out of %lu rid%s",
		   static_cast<long unsigned>(hits.cnt()),
		   static_cast<long unsigned>(in.size()),
		   (in.size()>1 ? "s" : ""));
    return hits.cnt();
} // ibis::part::evaluateRIDSet

// qString contains only two string values without any indication as what
// they represent.  This function actually uses the two string and decide
// what they are.  It first tries to match the left string against known
// column names of this part.  If the name matches one that is of type
// STRING or KEY, the search is performed on this column.  Otherwise the
// right string is compared against the column names, if a match is found,
// the search is performed on this column.  If both failed, the search
// returns no hit.
long ibis::part::lookforString(const ibis::qString& cmp,
			       ibis::bitvector& low) const {
    long ierr = 0;
    if (columns.empty() || nEvents == 0) return ierr;
    if (cmp.leftString() == 0) {
	low.set(0, nEvents);
	return ierr;
    }

    // try leftString()
    columnList::const_iterator it = columns.find(cmp.leftString());
    if (it != columns.end()) {
	if ((*it).second->type() == ibis::TEXT) {
	    const ibis::text* col =
		reinterpret_cast<const ibis::text*>((*it).second);
	    ierr = col->keywordSearch(cmp.rightString(), low);
	    if (ierr < 0)
		ierr = col->search(cmp.rightString(), low);
	    return ierr;
	}
	else if ((*it).second->type() == ibis::CATEGORY) {
	    const ibis::category* col =
		reinterpret_cast<const ibis::category*>((*it).second);
	    ierr = col->search(cmp.rightString(), low);
	    return ierr;
	}
    }

    // try rightString
    it = columns.find(cmp.rightString());
    if (it != columns.end()) {
	if ((*it).second->type() == ibis::TEXT) {
	    ibis::text* col = (ibis::text*)(*it).second;
	    ierr = col->keywordSearch(cmp.leftString(), low);
	    if (ierr < 0)
		ierr = col->search(cmp.rightString(), low);
	    return ierr;
	}
	else if ((*it).second->type() == ibis::CATEGORY) {
	    ibis::category* col = (ibis::category*)(*it).second;
	    ierr = col->search(cmp.leftString(), low);
	    return ierr;
	}
    }

    // no match -- no hit
    low.set(0, nEvents);
    return ierr;
} // ibis::part::lookforString

long ibis::part::lookforString(const ibis::qString& cmp) const {
    long ret = 0;
    if (columns.empty() || nEvents == 0)
	return ret;
    if (cmp.leftString() == 0)
	return ret;

    // try leftString()
    columnList::const_iterator it = columns.find(cmp.leftString());
    if (it != columns.end()) {
	if ((*it).second->type() == ibis::TEXT) {
	    ibis::text* col = (ibis::text*)(*it).second;
	    ret = col->keywordSearch(cmp.rightString());
	    if (ret < 0)
		ret = col->search(cmp.rightString());
	    return ret;
	}
	else if ((*it).second->type() == ibis::CATEGORY) {
	    ibis::category* col = (ibis::category*)(*it).second;
	    ret = col->search(cmp.rightString());
	    return ret;
	}
    }

    // try rightString
    it = columns.find(cmp.rightString());
    if (it != columns.end()) {
	if ((*it).second->type() == ibis::TEXT) {
	    ibis::text* col = (ibis::text*)(*it).second;
	    ret = col->keywordSearch(cmp.leftString());
	    if (ret < 0)
		ret = col->search(cmp.rightString());
	    return ret;
	}
	else if ((*it).second->type() == ibis::CATEGORY) {
	    ibis::category* col = (ibis::category*)(*it).second;
	    ret = col->search(cmp.leftString());
	    return ret;
	}
    }

    // no match -- no hit
    return ret;
} // ibis::part::lookforString

/// Determine the records that have the exact string values.  Actual work
/// done in the function search of the string-valued column.  It
/// produces no hit if the name is not a string-valued column.
long ibis::part::lookforString(const ibis::qMultiString& cmp,
			       ibis::bitvector& low) const {
    int ierr = 0;
    if (columns.empty() || nEvents == 0) return ierr;

    columnList::const_iterator it = columns.find(cmp.colName());
    if (it != columns.end()) {
	if ((*it).second->type() == ibis::TEXT) {
	    ibis::text* col = (ibis::text*)(*it).second;
	    ierr = col->search(cmp.valueList(), low);
	    return ierr;
	}
	else if ((*it).second->type() == ibis::CATEGORY) {
	    ibis::category* col = (ibis::category*)(*it).second;
	    ierr = col->search(cmp.valueList(), low);
	    return ierr;
	}
    }

    // no match -- no hit
    low.set(0, nEvents);
    return ierr;
} // ibis::part::lookforString

long ibis::part::lookforString(const ibis::qMultiString& cmp) const {
    long ret = 0;
    if (columns.empty() || nEvents == 0)
	return ret;

    columnList::const_iterator it = columns.find(cmp.colName());
    if (it != columns.end()) {
	if ((*it).second->type() == ibis::TEXT) {
	    ibis::text* col = (ibis::text*)(*it).second;
	    ret = col->search(cmp.valueList());
	    return ret;
	}
	else if ((*it).second->type() == ibis::CATEGORY) {
	    ibis::category* col = (ibis::category*)(*it).second;
	    ret = col->search(cmp.valueList());
	    return ret;
	}
    }

    // no match -- no hit
    return ret;
} // ibis::part::lookforString

// simply pass the job to the named column
long ibis::part::estimateRange(const ibis::qContinuousRange& cmp) const {
    long ret = 0;
    if (columns.empty() || nEvents == 0)
	return ret;
    if (cmp.colName() == 0)
	return ret;

    columnList::const_iterator it = columns.find(cmp.colName());
    if (it != columns.end()) {
	ret = (*it).second->estimateRange(cmp);
    }
    else {
	logWarning("estimateRange", "unable to find a "
		   "column named %s", cmp.colName());
    }

    LOGGER(8) << "ibis::part[" << name() << "]::estimateRange("
	      << cmp << ") <= " << ret;
    return ret;
} // ibis::part::estimateRange

long ibis::part::estimateRange(const ibis::qDiscreteRange& cmp) const {
    long ret = 0;
    if (columns.empty() || nEvents == 0)
	return ret;
    if (cmp.colName() == 0)
	return ret;

    columnList::const_iterator it = columns.find(cmp.colName());
    if (it != columns.end()) {
	ret = (*it).second->estimateRange(cmp);
    }
    else {
	logWarning("estimateRange", "unable to find a column "
		   "named %s", cmp.colName());
    }

    LOGGER(8) << "ibis::part[" << name() << "]::estimateRange("
	      << cmp << ") <= " << ret;
    return ret;
} // ibis::part::estimateRange

// simply pass the job to the named column
long ibis::part::evaluateRange(const ibis::qContinuousRange& cmp,
			       const ibis::bitvector& mask,
			       ibis::bitvector& hits) const {
    long ierr = 0;
    if (columns.empty() || nEvents == 0) return ierr;

    if (cmp.colName() == 0) { // no hit
	hits.set(0, nEvents);
    }
    else {
	columnList::const_iterator it = columns.find(cmp.colName());
	if (it != columns.end()) {
	    ierr = (*it).second->evaluateRange(cmp, mask, hits);
	}
	else {
	    logWarning("evaluateRange", "unable to find a column "
		       "named %s", cmp.colName());
	    hits.set(0, nEvents);
	}
    }

    LOGGER(8) << "ibis::part[" << name() << "]::evaluateRange("
	      << cmp << "), ierr = " << ierr;
    return ierr;
} // ibis::part::evaluateRange

long ibis::part::evaluateRange(const ibis::qDiscreteRange& cmp,
			       const ibis::bitvector& mask,
			       ibis::bitvector& hits) const {
    long ierr = 0;
    if (columns.empty() || nEvents == 0) return ierr;

    if (cmp.colName() == 0) { // no hit
	hits.set(0, nEvents);
    }
    else {
	columnList::const_iterator it = columns.find(cmp.colName());
	if (it != columns.end()) {
	    ierr = (*it).second->evaluateRange(cmp, mask, hits);
	}
	else {
	    logWarning("evaluateRange", "unable to find a column "
		       "named %s", cmp.colName());
	    hits.set(0, nEvents);
	}
    }

    LOGGER(8) << "ibis::part[" << name() << "]::evaluateRange("
	      << cmp << "), ierr = " << ierr;
    return ierr;
} // ibis::part::evaluateRange

// simply pass the job to the named column
long ibis::part::estimateRange(const ibis::qContinuousRange& cmp,
			       ibis::bitvector& low,
			       ibis::bitvector& high) const {
    long ierr = 0;
    if (columns.empty() || nEvents == 0) return ierr;

    if (cmp.colName() == 0) { // no hit
	low.set(0, nEvents);
	high.set(0, nEvents);
    }
    else {
	columnList::const_iterator it = columns.find(cmp.colName());
	if (it != columns.end()) {
	    ierr = (*it).second->estimateRange(cmp, low, high);
	    if (amask.size() == low.size()) {
		low &= amask;
		if (amask.size() == high.size())
		    high &= amask;
	    }
	}
	else {
	    logWarning("estimateRange", "unable to find a column "
		       "named %s", cmp.colName());
	    high.set(0, nEvents);
	    low.set(0, nEvents);
	}
    }

    if (high.size() == low.size() && high.cnt() > low.cnt()) {
	LOGGER(8) << "ibis::part[" << name() << "]::estimateRange("
		  << cmp << ") --> [" << low.cnt()
		  << ", " << high.cnt() << "]";
    }
    else {
	LOGGER(8) << "ibis::part[" << name() << "]::estimateRange("
		  << cmp << ") = " << low.cnt();
    }
    return ierr;
} // ibis::part::estimateRange

long ibis::part::estimateRange(const ibis::qDiscreteRange& cmp,
			       ibis::bitvector& low,
			       ibis::bitvector& high) const {
    long ierr = 0;
    if (columns.empty() || nEvents == 0) return ierr;

    if (cmp.colName() == 0) { // no hit
	low.set(0, nEvents);
	high.set(0, nEvents);
    }
    else {
	columnList::const_iterator it = columns.find(cmp.colName());
	if (it != columns.end()) {
	    ierr = (*it).second->estimateRange(cmp, low, high);
	    if (amask.size() == low.size()) {
		low &= amask;
		if (amask.size() == high.size())
		    high &= amask;
	    }
	}
	else {
	    logWarning("estimateRange", "unable to find a column "
		       "named %s", cmp.colName());
	    high.set(0, nEvents);
	    low.set(0, nEvents);
	}
    }

    if (high.size() == low.size() && high.cnt() > low.cnt()) {
	LOGGER(8) << "ibis::part[" << name() << "]::estimateRange("
		  << cmp << ") --> [" << low.cnt()
		  << ", " << high.cnt() << "]";
    }
    else {
	LOGGER(8) << "ibis::part[" << name() << "]::estimateRange("
		  << cmp << ") = " << low.cnt();
    }
    return ierr;
} // ibis::part::estimateRange

double ibis::part::estimateCost(const ibis::qContinuousRange& cmp) const {
    double ret = 0;
    if (columns.empty() || nEvents == 0)
	return ret;
    if (cmp.colName() == 0)
	return ret;

    columnList::const_iterator it = columns.find(cmp.colName());
    if (it != columns.end()) {
	ret = (*it).second->estimateCost(cmp);
    }
    else {
	logWarning("estimateCost", "unable to find a column named %s ",
		   cmp.colName());
    }
    return ret;
} // ibis::part::estimateCost

double ibis::part::estimateCost(const ibis::qDiscreteRange& cmp) const {
    double ret = 0;
    if (columns.empty() || nEvents == 0)
	return ret;
    if (cmp.colName() == 0)
	return ret;

    columnList::const_iterator it = columns.find(cmp.colName());
    if (it != columns.end()) {
	ret = (*it).second->estimateCost(cmp);
    }
    else {
	logWarning("estimateCost", "unable to find a column named %s",
		   cmp.colName());
    }
    return ret;
} // ibis::part::estimateCost

double ibis::part::estimateCost(const ibis::qString& cmp) const {
    double ret = 0;
    if (columns.empty() || nEvents == 0)
	return ret;
    if (cmp.leftString() == 0 || cmp.rightString() == 0)
	return ret;

    columnList::const_iterator it = columns.find(cmp.leftString());
    if (it == columns.end())
	it = columns.find(cmp.rightString());
    if (it != columns.end()) {
	ret = (*it).second->estimateCost(cmp);
    }
    else {
	logWarning("estimateCost", "unable to find a column named %s or %s",
		   cmp.leftString(), cmp.rightString());
    }
    return ret;
} // ibis::part::estimateCost

double ibis::part::estimateCost(const ibis::qMultiString& cmp) const {
    double ret = 0;
    if (columns.empty() || nEvents == 0)
	return ret;
    if (cmp.colName() == 0)
	return ret;

    columnList::const_iterator it = columns.find(cmp.colName());
    if (it != columns.end()) {
	ret = (*it).second->estimateCost(cmp);
    }
    else {
	logWarning("estimateCost", "unable to find a column named %s",
		   cmp.colName());
    }
    return ret;
} // ibis::part::estimateCost

/// Estimate the entries that matches a qAnyAny condition.  The actual
/// operations are performed as two types of range queries.
long ibis::part::estimateMatchAny(const ibis::qAnyAny& cmp,
				  ibis::bitvector& low,
				  ibis::bitvector& high) const {
    if (cmp.getPrefix() == 0 || cmp.getValues().empty()) return -1;
    if (nEvents == 0) return 0;

    low.set(0, nEvents);
    high.set(0, nEvents);
    const char *pref = cmp.getPrefix();
    const int   len = strlen(pref);
    const std::vector<double>& vals = cmp.getValues();
    columnList::const_iterator it = columns.lower_bound(pref);
    if (vals.size() > 1) { // multiple values, use discrete range query
	// TODO: to implement the proper functions to handle discrete
	// ranges in all index classes.
	// Temporary solution: change discrete range into a series of
	// equality conditions.
	while (it != columns.end() &&
	       0 == strnicmp((*it).first, pref, len)) {
	    // the column name has the specified prefix
	    ibis::bitvector msk, ltmp, htmp;
	    (*it).second->getNullMask(msk);
	    // 	    ibis::qDiscreteRange ex((*it).first, vals);
	    // 	    (*it).second->estimateRange(ex, ltmp, htmp);
	    ibis::qContinuousRange ex((*it).first, ibis::qExpr::OP_EQ,
				      vals[0]);
	    (*it).second->estimateRange(ex, ltmp, htmp);
	    for (uint32_t i = 1; i < vals.size(); ++ i) {
		ibis::bitvector ltmp2, htmp2;
		ibis::qContinuousRange ex2((*it).first, ibis::qExpr::OP_EQ,
					   vals[i]);
		(*it).second->estimateRange(ex2, ltmp2, htmp2);
		ltmp |= ltmp2;
		if (htmp2.size() == htmp.size())
		    htmp |= htmp2;
		else
		    htmp |= ltmp2;
	    }
	    ltmp &= msk;
	    low |= ltmp;
	    if (ltmp.size() == htmp.size()) {
		htmp &= msk;
		high |= htmp;
	    }
	    else {
		high |= ltmp;
	    }
	    ++ it; // check the next column
	}
    }
    else { // a single value, translate to simple equality query
	while (it != columns.end() &&
	       0 == strnicmp((*it).first, pref, len)) {
	    // the column name has the specified prefix
	    ibis::bitvector msk, ltmp, htmp;
	    (*it).second->getNullMask(msk);
	    ibis::qContinuousRange ex((*it).first, ibis::qExpr::OP_EQ,
				      vals.back());
	    (*it).second->estimateRange(ex, ltmp, htmp);
	    low |= ltmp;
	    if (ltmp.size() == htmp.size())
		high |= htmp;
	    else
		high |= ltmp;
	    ++ it; // check the next column
	}
    }
    return 0;
} // ibis::part::estimateMatchAny

/// Convert a set of numbers to an ibis::bitvector.
void ibis::part::numbersToBitvector(const std::vector<uint32_t>& rows,
				    ibis::bitvector& msk) const {
    if (rows.size() > 1) {
	array_t<uint32_t> r(rows.size());
	std::copy(rows.begin(), rows.end(), r.begin());
	std::sort(r.begin(), r.end());
	for (size_t i = 0; i < rows.size() && r[i] < nEvents; ++ i)
	    msk.setBit(r[i], 1);
    }
    else {
	msk.appendFill(0, rows[0]-1);
	msk += 1;
    }
    msk.adjustSize(0, nEvents);
} // ibis::part::numbersToBitvector

/// Convert a set of range conditions to an ibis::bitvector.
/// @note The bitvector may include active as well as inactive rows.
void ibis::part::stringToBitvector(const char* conds,
				   ibis::bitvector& msk) const {
    ibis::query q(ibis::util::userName(), this);
    q.setWhereClause(conds);
    q.getExpandedHits(msk);
} // ibis::part::stringToBitvector

// sequential scan without a mask -- the default mask is the NULL mask, in
// other word, all non-null entries are scaned
long ibis::part::doScan(const ibis::qRange& cmp,
			ibis::bitvector& hits) const {
    if (columns.empty() || nEvents == 0)
	return 0;
    if (cmp.colName() == 0)
	return 0;

    columnList::const_iterator it = columns.find(cmp.colName());
    if (it == columns.end()) {
	logWarning("doScan", "unable to find column %s "
		   "in the data partition", cmp.colName());
	hits.clear();
	return 0;
    }

    ibis::bitvector mask;
    (*it).second->getNullMask(mask);
    if (amask.size() == mask.size())
	mask &= amask;
    return doScan(cmp, mask, hits);
} // ibis::part::doScan

// sequential scan with a mask -- perform comparison on the i'th element if
// mask[i] is set (mask[i] == 1)
long ibis::part::doScan(const ibis::qRange& cmp,
			const ibis::bitvector& mask,
			ibis::bitvector& hits) const {
    if (columns.empty() || nEvents == 0 || cmp.colName() == 0 ||
	mask.size() == 0 || mask.cnt() == 0)
	return 0;

    columnList::const_iterator it = columns.find(cmp.colName());
    if (it == columns.end()) {
	logWarning("doScan", "unable to find named column %s "
		   "in the data partition", cmp.colName());
	return -1;
    }

    char* file = (*it).second->dataFileName();
    if (file == 0) {
	logWarning("doScan", "unable to locate the vertically "
		   "partitioned data file");
	return -2;
    }
    if ((*it).second->elementSize() > 0) {
	off_t fsize = ibis::util::getFileSize(file);
	if (fsize <= 0 || (unsigned long)fsize !=
	    (*it).second->elementSize() * nEvents) {
	    LOGGER(0) << "Warning -- part[" << (m_name?m_name:"?")
		      << "]::doScan(" << cmp
		      << ") can not proceed because of missing data file \""
		      << file << "\" or unexpected file size (" << fsize << ")";
	    delete [] file;
	    return -3;
	}
    }
    else {
	return -4;
    }

    long ierr = 0;
    switch ((*it).second->type()) {
    default:
	logWarning("doScan", "unable to process data type %d (%s)",
		   (*it).second->type(),
		   ibis::TYPESTRING[(int)(*it).second->type()]);
	hits.set(0, nEvents);
	ierr = -5;
	break;

    case ibis::CATEGORY: {
	//*** NOTE that there should not be any qRange query on columns of
	//*** type KEY or STRING.  However, they are implemented here to
	//*** make the selfTest code a little simpler!  This should be
	//*** corrected in the future.
	ibis::bitvector tmp;
	if (cmp.getType() == ibis::qExpr::RANGE)
	    (*it).second->estimateRange
		(reinterpret_cast<const ibis::qContinuousRange&>(cmp),
		 hits, tmp);
	else
	    (*it).second->estimateRange
		(reinterpret_cast<const ibis::qDiscreteRange&>(cmp),
		 hits, tmp);
	hits &= mask;
	break;}

    case ibis::TEXT: { // treat the values as row numbers
	if (cmp.getType() == ibis::qExpr::RANGE) {
	    double tmp = reinterpret_cast<const ibis::qContinuousRange&>
		(cmp).leftBound();
	    const uint32_t left = (tmp<=0.0 ? 0 : static_cast<uint32_t>(tmp));
	    tmp = reinterpret_cast<const ibis::qContinuousRange&>
		(cmp).rightBound();
	    uint32_t right = (tmp <= left ? left :
			      static_cast<uint32_t>(tmp));
	    if (right > nEvents)
		right = nEvents;
	    for (uint32_t i = left; i < right; ++ i)
		hits.setBit(i, 1);
	}
	else {
	    const std::vector<double>& vals =
		reinterpret_cast<const ibis::qDiscreteRange&>
		(cmp).getValues();
	    for (uint32_t i = 0; i < vals.size(); ++ i) {
		if (vals[i] >= 0 && vals[i] < nEvents)
		    hits.setBit(static_cast<unsigned>(vals[i]), 1);
	    }
	}
	hits.adjustSize(0, nEvents);
	hits &= mask;
	break;}

    case ibis::LONG: {
	array_t<int64_t> intarray;
	if (ibis::fileManager::instance().getFile(file, intarray) == 0) {
	    if (cmp.getType() == ibis::qExpr::RANGE) {
		const ibis::qContinuousRange& tmp =
		    static_cast<const ibis::qContinuousRange&>(cmp);
		ibis::qContinuousRange rng(tmp.leftBound(), tmp.leftOperator(),
					   tmp.colName(), tmp.rightOperator(),
					   tmp.rightBound());
		rng.foldBoundaries();
		ierr = doScan(intarray, rng, mask, hits);
	    }
	    else {
		ierr = doCompare(intarray, mask, hits, cmp);
	    }
	}
	else {
	    ierr = doCompare<int64_t>(file, mask, hits, cmp);
	}
	break;}

    case ibis::ULONG: {
	array_t<uint64_t> intarray;
	if (ibis::fileManager::instance().getFile(file, intarray) == 0) {
	    if (cmp.getType() == ibis::qExpr::RANGE) {
		const ibis::qContinuousRange& tmp =
		    static_cast<const ibis::qContinuousRange&>(cmp);
		ibis::qContinuousRange rng(tmp.leftBound(), tmp.leftOperator(),
					   tmp.colName(), tmp.rightOperator(),
					   tmp.rightBound());
		rng.foldUnsignedBoundaries();
		ierr = doScan(intarray, rng, mask, hits);
	    }
	    else {
		ierr = doCompare(intarray, mask, hits, cmp);
	    }
	}
	else {
	    ierr = doCompare<uint64_t>(file, mask, hits, cmp);
	}
	break;}

    case ibis::INT: {
	array_t<int32_t> intarray;
	if (ibis::fileManager::instance().getFile(file, intarray) == 0) {
	    if (cmp.getType() == ibis::qExpr::RANGE) {
		const ibis::qContinuousRange& tmp =
		    static_cast<const ibis::qContinuousRange&>(cmp);
		ibis::qContinuousRange rng(tmp.leftBound(), tmp.leftOperator(),
					   tmp.colName(), tmp.rightOperator(),
					   tmp.rightBound());
		rng.foldBoundaries();
		ierr = doScan(intarray, rng, mask, hits);
	    }
	    else {
		ierr = doCompare(intarray, mask, hits, cmp);
	    }
	}
	else {
	    ierr = doCompare<int32_t>(file, mask, hits, cmp);
	}
	break;}

    case ibis::UINT: {
	array_t<uint32_t> intarray;
	if (ibis::fileManager::instance().getFile(file, intarray) == 0) {
	    if (cmp.getType() == ibis::qExpr::RANGE) {
		const ibis::qContinuousRange& tmp =
		    static_cast<const ibis::qContinuousRange&>(cmp);
		ibis::qContinuousRange rng(tmp.leftBound(), tmp.leftOperator(),
					   tmp.colName(), tmp.rightOperator(),
					   tmp.rightBound());
		rng.foldUnsignedBoundaries();
		ierr = doScan(intarray, rng, mask, hits);
	    }
	    else {
		ierr = doCompare(intarray, mask, hits, cmp);
	    }
	}
	else {
	    ierr = doCompare<uint32_t>(file, mask, hits, cmp);
	}
	break;}

    case ibis::SHORT: {
	array_t<int16_t> intarray;
	if (ibis::fileManager::instance().getFile(file, intarray) == 0) {
	    if (cmp.getType() == ibis::qExpr::RANGE) {
		const ibis::qContinuousRange& tmp =
		    static_cast<const ibis::qContinuousRange&>(cmp);
		ibis::qContinuousRange rng(tmp.leftBound(), tmp.leftOperator(),
					   tmp.colName(), tmp.rightOperator(),
					   tmp.rightBound());
		rng.foldBoundaries();
		ierr = doScan(intarray, rng, mask, hits);
	    }
	    else {
		ierr = doCompare(intarray, mask, hits, cmp);
	    }
	}
	else {
	    ierr = doCompare<int16_t>(file, mask, hits, cmp);
	}
	break;}

    case ibis::USHORT: {
	array_t<uint16_t> intarray;
	if (ibis::fileManager::instance().getFile(file, intarray) == 0) {
	    if (cmp.getType() == ibis::qExpr::RANGE) {
		const ibis::qContinuousRange& tmp =
		    static_cast<const ibis::qContinuousRange&>(cmp);
		ibis::qContinuousRange rng(tmp.leftBound(), tmp.leftOperator(),
					   tmp.colName(), tmp.rightOperator(),
					   tmp.rightBound());
		rng.foldUnsignedBoundaries();
		ierr = doScan(intarray, rng, mask, hits);
	    }
	    else {
		ierr = doCompare(intarray, mask, hits, cmp);
	    }
	}
	else {
	    ierr = doCompare<uint16_t>(file, mask, hits, cmp);
	}
	break;}

    case ibis::BYTE: {
	array_t<char> intarray;
	if (ibis::fileManager::instance().getFile(file, intarray) == 0) {
	    if (cmp.getType() == ibis::qExpr::RANGE) {
		const ibis::qContinuousRange& tmp =
		    static_cast<const ibis::qContinuousRange&>(cmp);
		ibis::qContinuousRange rng(tmp.leftBound(), tmp.leftOperator(),
					   tmp.colName(), tmp.rightOperator(),
					   tmp.rightBound());
		rng.foldBoundaries();
		ierr = doScan(intarray, rng, mask, hits);
	    }
	    else {
		ierr = doCompare(intarray, mask, hits, cmp);
	    }
	}
	else {
	    ierr = doCompare<char>(file, mask, hits, cmp);
	}
	break;}

    case ibis::UBYTE: {
	array_t<unsigned char> intarray;
	if (ibis::fileManager::instance().getFile(file, intarray) == 0) {
	    if (cmp.getType() == ibis::qExpr::RANGE) {
		const ibis::qContinuousRange& tmp =
		    static_cast<const ibis::qContinuousRange&>(cmp);
		ibis::qContinuousRange rng(tmp.leftBound(), tmp.leftOperator(),
					   tmp.colName(), tmp.rightOperator(),
					   tmp.rightBound());
		rng.foldUnsignedBoundaries();
		ierr = doScan(intarray, rng, mask, hits);
	    }
	    else {
		ierr = doCompare(intarray, mask, hits, cmp);
	    }
	}
	else {
	    ierr = doCompare<unsigned char>(file, mask, hits, cmp);
	}
	break;}

    case ibis::FLOAT: {
	array_t<float> floatarray;
	if (ibis::fileManager::instance().getFile(file, floatarray) == 0) {
	    if (cmp.getType() == ibis::qExpr::RANGE)
		ierr = doScan(floatarray,
			      static_cast<const ibis::qContinuousRange&>(cmp),
			      mask, hits);
	    else
		ierr = doCompare(floatarray, mask, hits, cmp);
	}
	else {
	    ierr = doCompare<float>(file, mask, hits, cmp);
	}
	break;}

    case ibis::DOUBLE: {
	array_t<double> doublearray;
	if (ibis::fileManager::instance().getFile(file, doublearray) == 0) {
	    if (cmp.getType() == ibis::qExpr::RANGE)
		ierr = doScan(doublearray,
			      static_cast<const ibis::qContinuousRange&>(cmp),
			      mask, hits);
	    else
		ierr = doCompare(doublearray, mask, hits, cmp);
	}
	else {
	    ierr = doCompare<double>(file, mask, hits, cmp);
	}
	break;}
    }
    delete [] file;

    if (hits.size() != nEvents) {
	if (ibis::gVerbose > 3) // could happen when no hits
	    logMessage("doScan", "result array contains %lu bits, reset "
		       "size to %lu", static_cast<long unsigned>(hits.size()),
		       static_cast<long unsigned>(nEvents));
	hits.adjustSize(0, nEvents); // append 0 bits or remove extra bits
    }
    LOGGER(12) << "ibis::part[" << name() << "]: comparison " << cmp
	       << " is evaluated to have " << hits.cnt()
	       << " hits out of " << mask.cnt() << " candidates";
    return ierr;
} // ibis::part::doScan

template <typename E>
long ibis::part::doScan(const array_t<E>& varr,
			const ibis::qRange& cmp,
			const ibis::bitvector& mask,
			ibis::bitvector& hits) const {
    for (ibis::bitvector::indexSet is = mask.firstIndexSet();
	 is.nIndices() > 0; ++ is) {
	const ibis::bitvector::word_t *iix = is.indices();
	if (is.isRange()) {
	    const uint32_t last = (varr.size()>=iix[1] ? iix[1] : varr.size());
	    for (uint32_t i = iix[0]; i < last; ++ i) {
		if (cmp.inRange(varr[i])) {
		    hits.setBit(i, 1);
#if defined(DEBUG) && DEBUG + 0 > 1
		    LOGGER(0) << varr[i] << " is in " << cmp;
#endif
		}
	    }
	}
	else {
	    for (uint32_t i = 0; i < is.nIndices(); ++ i) {
		if (iix[i] < varr.size() && cmp.inRange(varr[iix[i]])) {
		    hits.setBit(i, 1);
#if defined(DEBUG) && DEBUG + 0 > 1
		    LOGGER(0) << varr[iix[i]] << " is in " << cmp;
#endif
		}
	    }
	}
    } // main loop

    hits.adjustSize(0, mask.size());
    return hits.cnt();
} // ibis::part::doScan

// sequential scan with a mask -- perform the negative comparison on the
// i'th element if mask[i] is set (mask[i] == 1)
long ibis::part::negativeScan(const ibis::qRange& cmp,
			      const ibis::bitvector& mask,
			      ibis::bitvector& hits) const {
    if (columns.empty() || nEvents == 0 || cmp.colName() == 0 ||
	mask.size() == 0 || mask.cnt() == 0)
	return 0;

    columnList::const_iterator it = columns.find(cmp.colName());
    if (it == columns.end()) {
	logWarning("negativeScan", "unable to find named column %s "
		   "in the data partition", cmp.colName());
	return -1;
    }

    char* file = (*it).second->dataFileName();
    if (file == 0) {
	logWarning("negativeScan", "unable to locate the vertically "
		   "partitioned data file");
	return -2;
    }
    if ((*it).second->elementSize() > 0) {
	off_t fsize = ibis::util::getFileSize(file);
	if (fsize <= 0 || (unsigned long)fsize !=
	    (*it).second->elementSize() * nEvents) {
	    LOGGER(0) << "Warning -- part[" << (m_name?m_name:"?")
		      << "]::negativeScan(" << cmp << "can not proceed"
		"because of missing data file \"" << file << "\"";
	    delete [] file;
	    return -3;
	}
    }

    long ierr = -4;
    switch ((*it).second->type()) {
    default:
	logWarning("negativeScan",
		   "unable to process data type %d (%s)",
		   (*it).second->type(),
		   ibis::TYPESTRING[(int)(*it).second->type()]);
	hits.set(0, nEvents);
	break;

    case CATEGORY: {
	ibis::bitvector tmp;
	if (cmp.getType() == ibis::qExpr::RANGE)
	    (*it).second->estimateRange
		(reinterpret_cast<const ibis::qContinuousRange&>(cmp),
		 hits, tmp);
	else
	    (*it).second->estimateRange
		(reinterpret_cast<const ibis::qDiscreteRange&>(cmp),
		 hits, tmp);
	hits &= mask;
	ierr = 0;
	break;}

    case ibis::LONG: {
	array_t<int64_t> intarray;
	if (ibis::fileManager::instance().getFile(file, intarray) == 0) {
	    ierr = negativeCompare(intarray, mask, hits, cmp);
	}
	else {
	    ierr = negativeCompare<int64_t>(file, mask, hits, cmp);
	}
	break;}

    case ibis::ULONG: {
	array_t<uint64_t> intarray;
	if (ibis::fileManager::instance().getFile(file, intarray) == 0) {
	    ierr = negativeCompare(intarray, mask, hits, cmp);
	}
	else {
	    ierr = negativeCompare<uint64_t>(file, mask, hits, cmp);
	}
	break;}

    case ibis::INT: {
	array_t<int32_t> intarray;
	if (ibis::fileManager::instance().getFile(file, intarray) == 0) {
	    ierr = negativeCompare(intarray, mask, hits, cmp);
	}
	else {
	    ierr = negativeCompare<int32_t>(file, mask, hits, cmp);
	}
	break;}

    case TEXT:
    case ibis::UINT: {
	array_t<uint32_t> intarray;
	if (ibis::fileManager::instance().getFile(file, intarray) == 0) {
	    ierr = negativeCompare(intarray, mask, hits, cmp);
	}
	else {
	    ierr = negativeCompare<uint32_t>(file, mask, hits, cmp);
	}
	break;}

    case ibis::SHORT: {
	array_t<int16_t> intarray;
	if (ibis::fileManager::instance().getFile(file, intarray) == 0) {
	    ierr = negativeCompare(intarray, mask, hits, cmp);
	}
	else {
	    ierr = negativeCompare<int16_t>(file, mask, hits, cmp);
	}
	break;}

    case ibis::USHORT: {
	array_t<uint16_t> intarray;
	if (ibis::fileManager::instance().getFile(file, intarray) == 0) {
	    ierr = negativeCompare(intarray, mask, hits, cmp);
	}
	else {
	    ierr = negativeCompare<uint16_t>(file, mask, hits, cmp);
	}
	break;}

    case ibis::BYTE: {
	array_t<signed char> intarray;
	if (ibis::fileManager::instance().getFile(file, intarray) == 0) {
	    ierr = negativeCompare(intarray, mask, hits, cmp);
	}
	else {
	    ierr = negativeCompare<signed char>(file, mask, hits, cmp);
	}
	break;}

    case ibis::UBYTE: {
	array_t<unsigned char> intarray;
	if (ibis::fileManager::instance().getFile(file, intarray) == 0) {
	    ierr = negativeCompare(intarray, mask, hits, cmp);
	}
	else {
	    ierr = negativeCompare<unsigned char>(file, mask, hits, cmp);
	}
	break;}

    case ibis::FLOAT: {
	array_t<float> floatarray;
	if (ibis::fileManager::instance().getFile(file, floatarray) == 0) {
	    ierr = negativeCompare(floatarray, mask, hits, cmp);
	}
	else {
	    ierr = negativeCompare<float>(file, mask, hits, cmp);
	}
	break;}

    case ibis::DOUBLE: {
	array_t<double> doublearray;
	if (ibis::fileManager::instance().getFile(file, doublearray) == 0) {
	    ierr = negativeCompare(doublearray, mask, hits, cmp);
	}
	else {
	    ierr = negativeCompare<double>(file, mask, hits, cmp);
	}
	break;}
    }
    delete [] file;

    if (hits.size() != nEvents) {
	logError("negativeScan",
		 "result array contains %lu bits, reset size to %lu",
		 static_cast<long unsigned>(hits.size()),
		 static_cast<long unsigned>(nEvents));
	// hits.adjustSize(0, nEvents); // append 0 bits or remove extra bits
    }
    LOGGER(12) << "ibis::part[" << name() << "]: negative comparison "
	       << cmp << " is evaluated to have " << hits.cnt()
	       << " hits out of " << mask.cnt() << " candidates";
    return ierr;
} // ibis::part::negativeScan

float ibis::part::getUndecidable(const ibis::qContinuousRange& cmp,
				 ibis::bitvector& iffy) const {
    float ret = 0;
    if (columns.empty() || nEvents == 0)
	return ret;
    if (cmp.colName() == 0)
	return ret;

    columnList::const_iterator it = columns.find(cmp.colName());
    if (it != columns.end()) {
	ret = (*it).second->getUndecidable(cmp, iffy);
    }
    else {
	logWarning("getUndecidable", "unable to find a column named %s",
		   cmp.colName());
    }

    LOGGER(8) << "ibis::part[" << name() << "]::getUndecidable("
	      << cmp << ") get a bitvector with " << iffy.cnt()
	      << " nonzeros, " << ret*100
	      << " per cent of them might be in the range";
    return ret;
} // ibis::part::getUndecidable

float ibis::part::getUndecidable(const ibis::qDiscreteRange& cmp,
				 ibis::bitvector& iffy) const {
    float ret = 0;
    if (columns.empty() || nEvents == 0 || cmp.colName() == 0)
	return ret;

    columnList::const_iterator it = columns.find(cmp.colName());
    if (it != columns.end()) {
	ret = (*it).second->getUndecidable(cmp, iffy);
    }
    else {
	logWarning("getUndecidable", "unable to find a column named %s",
		   cmp.colName());
    }

    LOGGER(8) << "ibis::part[" << name() << "]::getUndecidable("
	      << cmp << ") get a bitvector with " << iffy.cnt()
	      << " nonzeros, " << ret*100
	      << " per cent of them might be in the range";
    return ret;
} // ibis::part::getUndecidable

// sequential scan without a mask.  Use the NULL mask of all attributes as
// the default mask.
long ibis::part::doScan(const ibis::compRange& cmp,
			ibis::bitvector& hits) const {
    if (columns.empty() || nEvents == 0)
	return 0;

    ibis::bitvector mask;
    ibis::compRange::barrel bar;
    if (cmp.getLeft())
	bar.recordVariable(static_cast<const ibis::compRange::term*>
			   (cmp.getLeft()));
    if (cmp.getRight())
	bar.recordVariable(static_cast<const ibis::compRange::term*>
			   (cmp.getRight()));
    if (cmp.getTerm3())
	bar.recordVariable(static_cast<const ibis::compRange::term*>
			   (cmp.getTerm3()));

    mask.set(1, nEvents); // initialize mask to have all set bit
    for (uint32_t i = 0; i < bar.size(); ++i) {
	// go through the list of variables to determine the real null mask
	columnList::const_iterator it = columns.find(bar.name(i));
	if (it != columns.end()) {
	    ibis::bitvector tmp;
	    (*it).second->getNullMask(tmp);
	    mask &= tmp;
	}
	else {
	    logWarning("doScan", "unable to find column %s "
		       "in the partition", bar.name(i));
	    hits.clear();
	    return 0;
	}
    }

    return doScan(cmp, mask, hits, &bar);
} // ibis::part::doScan

// sequential scan with a mask -- perform comparison on the i'th element if
// mask[i] is set (mask[i] == 1)
// bar is used as workspace to store the values of the variables read
long ibis::part::doScan(const ibis::compRange& cmp,
			const ibis::bitvector& mask,
			ibis::bitvector& hits,
			ibis::compRange::barrel* bar) const {
    if (columns.empty() || nEvents == 0)
	return 0;
    ibis::horometer timer;
    if (ibis::gVerbose > 1)
	timer.start();

    ibis::compRange::barrel* vlist = bar;
    ibis::compRange::barrel* barr = 0;
    long ierr;
    if (bar == 0) { // need to collect the names of all variables
	barr = new ibis::compRange::barrel;
	if (cmp.getLeft())
	    barr->recordVariable(static_cast<const ibis::compRange::term*>
				 (cmp.getLeft()));
	if (cmp.getRight())
	    barr->recordVariable(static_cast<const ibis::compRange::term*>
				 (cmp.getRight()));
	if (cmp.getTerm3())
	    barr->recordVariable(static_cast<const ibis::compRange::term*>
				 (cmp.getTerm3()));
	vlist = barr;
    }

    if (vlist->size() > 0) {
	// use a list of ibis::fileManager::storage as the primary means for
	// accessing the actual values, if we can not generate the storage
	// object, try to open the named file directly
	std::vector< ibis::fileManager::storage* > stores(vlist->size(), 0);
	std::vector< ibis::column* > cols(vlist->size(), 0);
	std::vector< int > fdes(vlist->size(), -1);
	for (uint32_t i = 0; i < vlist->size(); ++i) {
	    columnList::const_iterator it = columns.find(vlist->name(i));
	    if (it != columns.end()) {
		cols[i] = (*it).second;
		char* file = (*it).second->dataFileName();
		if (0 != ibis::fileManager::instance().
		    getFile(file, &(stores[i]))) {
		    delete stores[i];
		    stores[i] = 0;
		    fdes[i] = UnixOpen(file, OPEN_READONLY);
		    if (fdes[i] < 0) {
			for (uint32_t j = 0; j < i; ++j) {
			    delete stores[j];
			    if (fdes[j] >= 0)
				UnixClose(fdes[j]);
			}
			logWarning("doScan", "unable to open file "
				   "\"%s\"", file);
			delete [] file;
			if (bar == 0)
			    delete barr;
			return 0;
		    }
		    delete [] file;
#if defined(_WIN32) && defined(_MSC_VER)
		    (void)_setmode(fdes[i], _O_BINARY);
#endif
		}
	    }
	    else {
		for (uint32_t j = 0; j < i; ++j) {
		    if (fdes[j] >= 0)
			UnixClose(fdes[j]);
		    logWarning("doScan", "no attribute with name "
			       "\"%s\"", vlist->name(i));
		    if (bar == 0)
			delete barr;
		    return 0;
		}
	    }
	}

	for (uint32_t i = 0; i < vlist->size(); ++i) {
	    if (stores[i])
		stores[i]->beginUse();
	}

	const bool uncomp = ((mask.size() >> 8) < mask.cnt());
	if (uncomp) { // use uncompressed hits internally
	    hits.set(0, mask.size());
	    hits.decompress();
	}
	else {
	    hits.clear();
	    hits.reserve(mask.size(), mask.cnt());
	}

	// attempt to feed the values into vlist and evaluate the arithmetic
	// expression through ibis::compRange::inRange
	ibis::bitvector::indexSet idx = mask.firstIndexSet();
	const ibis::bitvector::word_t *iix = idx.indices();
	while (idx.nIndices() > 0) {
	    if (idx.isRange()) {
		// move the file pointers of open files
		for (uint32_t i = 0; i < vlist->size(); ++i) {
		    if (fdes[i] >= 0) {
			uint32_t tmp1, tmp2;
			tmp1 = cols[i]->elementSize() * *iix;
			tmp2 = cols[i]->elementSize() * iix[1];
			ierr = UnixSeek(fdes[i], tmp1, SEEK_SET);
			if (ierr < 0) {
			    LOGGER(1) << "Warning -- ibis::part[" << (m_name ? m_name : "?")
				      << "]::doScan(" << cmp
				      << ") failed to seek to " << tmp1
				      << " in file " << fdes[i];
			    for (size_t k = 0; k < vlist->size(); ++k) {
				if (fdes[k] >= 0)
				    UnixClose(fdes[k]);
			    }
			    if (bar == 0) delete barr;
			    return -2;
			}
			ibis::fileManager::instance().recordPages(tmp1, tmp2);
		    }
		}
		for (uint32_t j = 0; j < idx.nIndices(); ++j) {
		    for (uint32_t i = 0; i < vlist->size(); ++i) {
			switch (cols[i]->type()) {
			case ibis::ULONG: {
			    // unsigned integer
			    uint64_t utmp;
			    if (stores[i]) {
				utmp = *(reinterpret_cast<uint64_t*>
					 (stores[i]->begin() +
					  sizeof(utmp) * (j + *iix)));
			    }
			    else {
				ierr = UnixRead(fdes[i], &utmp, sizeof(utmp));
				if (ierr < 0) {
				    LOGGER(1) << "Warning -- ibis::part["
					      << (m_name ? m_name : "?") << "]::doScan(" << cmp
					      << ") failed to read "
					      << "from file " << fdes[i];
				    for (size_t k = 0; k < vlist->size(); ++k) {
					if (fdes[k] >= 0)
					    UnixClose(fdes[k]);
				    }
				    if (bar == 0) delete barr;
				    return -3;
				}
			    }
			    vlist->value(i) = utmp;
			    break;
			}
			case ibis::LONG: {
			    // signed integer
			    int64_t itmp;
			    if (stores[i]) {
				itmp = *(reinterpret_cast<int64_t*>
					 (stores[i]->begin() +
					  sizeof(itmp) * (j + *iix)));
			    }
			    else {
				ierr = UnixRead(fdes[i], &itmp, sizeof(itmp));
				if (ierr < 0) {
				    LOGGER(1) << "Warning -- ibis::part["
					      << (m_name ? m_name : "?") << "]::doScan(" << cmp
					      << ") failed to read "
					      << "from file " << fdes[i];
				    for (size_t k = 0; k < vlist->size(); ++k) {
					if (fdes[k] >= 0)
					    UnixClose(fdes[k]);
				    }
				    if (bar == 0) delete barr;
				    return -3;
				}
			    }
			    vlist->value(i) = itmp;
			    break;
			}
			case ibis::CATEGORY:
			case ibis::UINT:
			case ibis::TEXT: {
			    // unsigned integer
			    uint32_t utmp;
			    if (stores[i]) {
				utmp = *(reinterpret_cast<uint32_t*>
					 (stores[i]->begin() +
					  sizeof(utmp) * (j + *iix)));
			    }
			    else {
				ierr = UnixRead(fdes[i], &utmp, sizeof(utmp));
				if (ierr < 0) {
				    LOGGER(1) << "Warning -- ibis::part["
					      << (m_name ? m_name : "?") << "]::doScan(" << cmp
					      << ") failed to read "
					      << "from file " << fdes[i];
				    for (size_t k = 0; k < vlist->size(); ++k) {
					if (fdes[k] >= 0)
					    UnixClose(fdes[k]);
				    }
				    if (bar == 0) delete barr;
				    return -3;
				}
			    }
			    vlist->value(i) = utmp;
			    break;
			}
			case ibis::INT: {
			    // signed integer
			    int32_t itmp;
			    if (stores[i]) {
				itmp = *(reinterpret_cast<int32_t*>
					 (stores[i]->begin() +
					  sizeof(itmp) * (j + *iix)));
			    }
			    else {
				ierr = UnixRead(fdes[i], &itmp, sizeof(itmp));
				if (ierr < 0) {
				    LOGGER(1) << "Warning -- ibis::part["
					      << (m_name ? m_name : "?") << "]::doScan(" << cmp
					      << ") failed to read "
					      << "from file " << fdes[i];
				    for (size_t k = 0; k < vlist->size(); ++k) {
					if (fdes[k] >= 0)
					    UnixClose(fdes[k]);
				    }
				    if (bar == 0) delete barr;
				    return -3;
				}
			    }
			    vlist->value(i) = itmp;
			    break;
			}
			case ibis::USHORT: {
			    // unsigned short integer
			    uint16_t utmp;
			    if (stores[i]) {
				utmp = *(reinterpret_cast<uint16_t*>
					 (stores[i]->begin() +
					  sizeof(utmp) * (j + *iix)));
			    }
			    else {
				ierr = UnixRead(fdes[i], &utmp, sizeof(utmp));
				if (ierr < 0) {
				    LOGGER(1) << "Warning -- ibis::part["
					      << (m_name ? m_name : "?") << "]::doScan(" << cmp
					      << ") failed to read "
					      << "from file " << fdes[i];
				    for (size_t k = 0; k < vlist->size(); ++k) {
					if (fdes[k] >= 0)
					    UnixClose(fdes[k]);
				    }
				    if (bar == 0) delete barr;
				    return -3;
				}
			    }
			    vlist->value(i) = utmp;
			    break;
			}
			case ibis::SHORT: {
			    // signed short integer
			    int16_t itmp;
			    if (stores[i]) {
				itmp = *(reinterpret_cast<int16_t*>
					 (stores[i]->begin() +
					  sizeof(itmp) * (j + *iix)));
			    }
			    else {
				ierr = UnixRead(fdes[i], &itmp, sizeof(itmp));
				if (ierr < 0) {
				    LOGGER(1) << "Warning -- ibis::part["
					      << (m_name ? m_name : "?") << "]::doScan(" << cmp
					      << ") failed to read "
					      << "from file " << fdes[i];
				    for (size_t k = 0; k < vlist->size(); ++k) {
					if (fdes[k] >= 0)
					    UnixClose(fdes[k]);
				    }
				    if (bar == 0) delete barr;
				    return -3;
				}
			    }
			    vlist->value(i) = itmp;
			    break;
			}
			case ibis::UBYTE: {
			    // unsigned char
			    unsigned char utmp;
			    if (stores[i]) {
				utmp = *(reinterpret_cast<unsigned char*>
					 (stores[i]->begin() +
					  sizeof(utmp) * (j + *iix)));
			    }
			    else {
				ierr = UnixRead(fdes[i], &utmp, sizeof(utmp));
				if (ierr < 0) {
				    LOGGER(1) << "Warning -- ibis::part["
					      << (m_name ? m_name : "?") << "]::doScan(" << cmp
					      << ") failed to read "
					      << "from file " << fdes[i];
				    for (size_t k = 0; k < vlist->size(); ++k) {
					if (fdes[k] >= 0)
					    UnixClose(fdes[k]);
				    }
				    if (bar == 0) delete barr;
				    return -3;
				}
			    }
			    vlist->value(i) = utmp;
			    break;
			}
			case ibis::BYTE: {
			    // signed char
			    signed char itmp;
			    if (stores[i]) {
				itmp = *(reinterpret_cast<signed char*>
					 (stores[i]->begin() +
					  sizeof(itmp) * (j + *iix)));
			    }
			    else {
				ierr = UnixRead(fdes[i], &itmp, sizeof(itmp));
				if (ierr < 0) {
				    LOGGER(1) << "Warning -- ibis::part["
					      << (m_name ? m_name : "?") << "]::doScan(" << cmp
					      << ") failed to read "
					      << "from file " << fdes[i];
				    for (size_t k = 0; k < vlist->size(); ++k) {
					if (fdes[k] >= 0)
					    UnixClose(fdes[k]);
				    }
				    if (bar == 0) delete barr;
				    return -3;
				}
			    }
			    vlist->value(i) = itmp;
			    break;
			}
			case ibis::FLOAT: {
			    // 4-byte IEEE floating-point values
			    float ftmp;
			    if (stores[i]) {
				ftmp = *(reinterpret_cast<float*>
					 (stores[i]->begin() +
					  sizeof(ftmp) * (j + *iix)));
			    }
			    else {
				ierr = UnixRead(fdes[i], &ftmp, sizeof(ftmp));
				if (ierr < 0) {
				    LOGGER(1) << "Warning -- ibis::part["
					      << (m_name ? m_name : "?") << "]::doScan(" << cmp
					      << ") failed to read "
					      << "from file " << fdes[i];
				    for (size_t k = 0; k < vlist->size(); ++k) {
					if (fdes[k] >= 0)
					    UnixClose(fdes[k]);
				    }
				    if (bar == 0) delete barr;
				    return -3;
				}
			    }
			    vlist->value(i) = ftmp;
			    break;
			}
			case ibis::DOUBLE: {
			    // 8-byte IEEE floating-point values
			    double dtmp;
			    if (stores[i]) {
				dtmp = *(reinterpret_cast<double*>
					 (stores[i]->begin() +
					  sizeof(dtmp) * (j + *iix)));
			    }
			    else {
				ierr = UnixRead(fdes[i], &dtmp, sizeof(dtmp));
				if (ierr < 0) {
				    LOGGER(1) << "Warning -- ibis::part["
					      << (m_name ? m_name : "?") << "]::doScan(" << cmp
					      << ") failed to read "
					      << "from file " << fdes[i];
				    for (size_t k = 0; k < vlist->size(); ++k) {
					if (fdes[k] >= 0)
					    UnixClose(fdes[k]);
				    }
				    if (bar == 0) delete barr;
				    return -3;
				}
			    }
			    vlist->value(i) = dtmp;
			    break;
			}
			case ibis::OID:
			default: {
			    logWarning("doScan", "unable to evaluate "
				       "attribute of type %d (name: %s)",
				       cols[i]->type(), cols[i]->name());
			    for (uint32_t k = 0; k < vlist->size(); ++k) {
				if (fdes[k] >= 0)
				    UnixClose(fdes[k]);
			    }
			    if (bar == 0)
				delete barr;
			    return 0;
			}
			}
		    } // for (uint32_t i = 0; i < vlist->size(); ++i)

		    if (cmp.inRange())
			hits.setBit(j + *iix, 1);
		} // for (uint32_t j = 0; j < idx.nIndices(); ++j)
	    }
	    else {
		for (uint32_t j = 0; j < idx.nIndices(); ++j) {
		    for (uint32_t i = 0; i < vlist->size(); ++i) {
			if (fdes[i] >= 0) {
			    // move the file pointers of open files
			    unsigned tmp1 = cols[i]->elementSize() * iix[j];
			    ierr = UnixSeek(fdes[i], tmp1, SEEK_SET);
			    if (ierr < 0) {
				LOGGER(1) << "Warning -- ibis::part["
					  << (m_name ? m_name : "?") << "]::doScan(" << cmp
					  << ") failed to seek to " << tmp1
					  << " in file " << fdes[i];
				for (size_t k = 0; k < vlist->size(); ++k) {
				    if (fdes[k] >= 0)
					UnixClose(fdes[k]);
				}
				if (bar == 0) delete barr;
				return -2;
			    }
			}

			switch (cols[i]->type()) {
			case ibis::ULONG: {
			    // unsigned integer
			    uint64_t utmp;
			    if (stores[i]) {
				utmp = *(reinterpret_cast<uint64_t*>
					 (stores[i]->begin() +
					  sizeof(utmp) * (iix[j])));
			    }
			    else {
				ierr = UnixRead(fdes[i], &utmp, sizeof(utmp));
				if (ierr < 0) {
				    LOGGER(1) << "Warning -- ibis::part["
					      << (m_name ? m_name : "?") << "]::doScan(" << cmp
					      << ") failed to read "
					      << "from file " << fdes[i];
				    for (size_t k = 0; k < vlist->size(); ++k) {
					if (fdes[k] >= 0)
					    UnixClose(fdes[k]);
				    }
				    if (bar == 0) delete barr;
				    return -3;
				}
			    }
			    vlist->value(i) = utmp;
			    break;
			}
			case ibis::LONG: {
			    // signed integer
			    int64_t itmp;
			    if (stores[i]) {
				itmp = *(reinterpret_cast<int64_t*>
					 (stores[i]->begin() +
					  sizeof(itmp) * (iix[j])));
			    }
			    else {
				ierr = UnixRead(fdes[i], &itmp, sizeof(itmp));
				if (ierr < 0) {
				    LOGGER(1) << "Warning -- ibis::part["
					      << (m_name ? m_name : "?") << "]::doScan(" << cmp
					      << ") failed to read "
					      << "from file " << fdes[i];
				    for (size_t k = 0; k < vlist->size(); ++k) {
					if (fdes[k] >= 0)
					    UnixClose(fdes[k]);
				    }
				    if (bar == 0) delete barr;
				    return -3;
				}
			    }
			    vlist->value(i) = itmp;
			    break;
			}
			case ibis::CATEGORY:
			case ibis::UINT:
			case ibis::TEXT: {
			    // unsigned integer
			    uint32_t utmp;
			    if (stores[i]) {
				utmp = *(reinterpret_cast<uint32_t*>
					 (stores[i]->begin() +
					  sizeof(utmp) * (iix[j])));
			    }
			    else {
				ierr = UnixRead(fdes[i], &utmp, sizeof(utmp));
				if (ierr < 0) {
				    LOGGER(1) << "Warning -- ibis::part["
					      << (m_name ? m_name : "?") << "]::doScan(" << cmp
					      << ") failed to read "
					      << "from file " << fdes[i];
				    for (size_t k = 0; k < vlist->size(); ++k) {
					if (fdes[k] >= 0)
					    UnixClose(fdes[k]);
				    }
				    if (bar == 0) delete barr;
				    return -3;
				}
			    }
			    vlist->value(i) = utmp;
			    break;
			}
			case ibis::INT: {
			    // signed integer
			    int32_t itmp;
			    if (stores[i]) {
				itmp = *(reinterpret_cast<int32_t*>
					 (stores[i]->begin() +
					  sizeof(itmp) * (iix[j])));
			    }
			    else {
				ierr = UnixRead(fdes[i], &itmp, sizeof(itmp));
				if (ierr < 0) {
				    LOGGER(1) << "Warning -- ibis::part["
					      << (m_name ? m_name : "?") << "]::doScan(" << cmp
					      << ") failed to read "
					      << "from file " << fdes[i];
				    for (size_t k = 0; k < vlist->size(); ++k) {
					if (fdes[k] >= 0)
					    UnixClose(fdes[k]);
				    }
				    if (bar == 0) delete barr;
				    return -3;
				}
			    }
			    vlist->value(i) = itmp;
			    break;
			}
			case ibis::USHORT: {
			    // unsigned short integer
			    uint16_t utmp;
			    if (stores[i]) {
				utmp = *(reinterpret_cast<uint16_t*>
					 (stores[i]->begin() +
					  sizeof(utmp) * (iix[j])));
			    }
			    else {
				ierr = UnixRead(fdes[i], &utmp, sizeof(utmp));
				if (ierr < 0) {
				    LOGGER(1) << "Warning -- ibis::part["
					      << (m_name ? m_name : "?") << "]::doScan(" << cmp
					      << ") failed to read "
					      << "from file " << fdes[i];
				    for (size_t k = 0; k < vlist->size(); ++k) {
					if (fdes[k] >= 0)
					    UnixClose(fdes[k]);
				    }
				    if (bar == 0) delete barr;
				    return -3;
				}
			    }
			    vlist->value(i) = utmp;
			    break;
			}
			case ibis::SHORT: {
			    // signed short integer
			    int16_t itmp;
			    if (stores[i]) {
				itmp = *(reinterpret_cast<int16_t*>
					 (stores[i]->begin() +
					  sizeof(itmp) * (iix[j])));
			    }
			    else {
				ierr = UnixRead(fdes[i], &itmp, sizeof(itmp));
				if (ierr < 0) {
				    LOGGER(1) << "Warning -- ibis::part["
					      << (m_name ? m_name : "?") << "]::doScan(" << cmp
					      << ") failed to read "
					      << "from file " << fdes[i];
				    for (size_t k = 0; k < vlist->size(); ++k) {
					if (fdes[k] >= 0)
					    UnixClose(fdes[k]);
				    }
				    if (bar == 0) delete barr;
				    return -3;
				}
			    }
			    vlist->value(i) = itmp;
			    break;
			}
			case ibis::UBYTE: {
			    // unsigned char
			    unsigned char utmp;
			    if (stores[i]) {
				utmp = *(reinterpret_cast<unsigned char*>
					 (stores[i]->begin() +
					  sizeof(utmp) * (iix[j])));
			    }
			    else {
				ierr = UnixRead(fdes[i], &utmp, sizeof(utmp));
				if (ierr < 0) {
				    LOGGER(1) << "Warning -- ibis::part["
					      << (m_name ? m_name : "?") << "]::doScan(" << cmp
					      << ") failed to read "
					      << "from file " << fdes[i];
				    for (size_t k = 0; k < vlist->size(); ++k) {
					if (fdes[k] >= 0)
					    UnixClose(fdes[k]);
				    }
				    if (bar == 0) delete barr;
				    return -3;
				}
			    }
			    vlist->value(i) = utmp;
			    break;
			}
			case ibis::BYTE: {
			    // signed char
			    signed char itmp;
			    if (stores[i]) {
				itmp = *(reinterpret_cast<signed char*>
					 (stores[i]->begin() +
					  sizeof(itmp) * (iix[j])));
			    }
			    else {
				ierr = UnixRead(fdes[i], &itmp, sizeof(itmp));
				if (ierr < 0) {
				    LOGGER(1) << "Warning -- ibis::part["
					      << (m_name ? m_name : "?") << "]::doScan(" << cmp
					      << ") failed to read "
					      << "from file " << fdes[i];
				    for (size_t k = 0; k < vlist->size(); ++k) {
					if (fdes[k] >= 0)
					    UnixClose(fdes[k]);
				    }
				    if (bar == 0) delete barr;
				    return -3;
				}
			    }
			    vlist->value(i) = itmp;
			    break;
			}
			case ibis::FLOAT: {
			    // 4-byte IEEE floating-point values
			    float ftmp;
			    if (stores[i]) {
				ftmp = *(reinterpret_cast<float*>
					 (stores[i]->begin() +
					  sizeof(ftmp) * (iix[j])));
			    }
			    else {
				ierr = UnixRead(fdes[i], &ftmp, sizeof(ftmp));
				if (ierr < 0) {
				    LOGGER(1) << "Warning -- ibis::part["
					      << (m_name ? m_name : "?") << "]::doScan(" << cmp
					      << ") failed to read "
					      << "from file " << fdes[i];
				    for (size_t k = 0; k < vlist->size(); ++k) {
					if (fdes[k] >= 0)
					    UnixClose(fdes[k]);
				    }
				    if (bar == 0) delete barr;
				    return -3;
				}
			    }
			    vlist->value(i) = ftmp;
			    break;
			}
			case ibis::DOUBLE: {
			    // 8-byte IEEE floating-point values
			    double dtmp;
			    if (stores[i]) {
				dtmp = *(reinterpret_cast<double*>
					 (stores[i]->begin() +
					  sizeof(dtmp) * (iix[j])));
			    }
			    else {
				ierr = UnixRead(fdes[i], &dtmp, sizeof(dtmp));
				if (ierr < 0) {
				    LOGGER(1) << "Warning -- ibis::part["
					      << (m_name ? m_name : "?") << "]::doScan(" << cmp
					      << ") failed to read "
					      << "from file " << fdes[i];
				    for (size_t k = 0; k < vlist->size(); ++k) {
					if (fdes[k] >= 0)
					    UnixClose(fdes[k]);
				    }
				    if (bar == 0) delete barr;
				    return -3;
				}
			    }
			    vlist->value(i) = dtmp;
			    break;
			}
			case ibis::OID:
			default: {
			    logWarning("doScan", "unable to evaluate "
				       "attribute of type %d (name: %s)",
				       cols[i]->type(), cols[i]->name());
			    for (size_t k = 0; k < vlist->size(); ++k) {
				if (fdes[k] >= 0)
				    UnixClose(fdes[k]);
			    }
			    if (bar == 0)
				delete barr;
			    return 0;
			}
			}
		    } // for (uint32_t i = 0; i < vlist->size(); ++i)

		    if (cmp.inRange())
			hits.setBit(iix[j], 1);
		} // for (uint32_t j = 0; j < idx.nIndices(); ++j)
	    }

	    ++ idx;
	} // while (idx.nIndices() > 0)

	if (uncomp)
	    hits.compress();
	else if (hits.size() < nEvents)
	    hits.setBit(nEvents-1, 0);
	for (uint32_t i = 0; i < vlist->size(); ++i) {
	    if (stores[i])
		stores[i]->endUse();
	    if (fdes[i] >= 0)
		UnixClose(fdes[i]);
	}
    }
    else { // a constant expression
	if (cmp.inRange())
	    hits.copy(mask);
	else
	    hits.set(mask.size(), 0);
    }

    if (bar == 0) // we have declared our own copy of ibis::compRange::barrel
	delete barr;

    if (ibis::gVerbose > 1) {
	timer.stop();
	ibis::util::logger lg(1);
	lg.buffer() << "ibis::part[" << (m_name ? m_name : "?")
		    << "]::doScan -- evaluating "
		    << cmp << " on " << mask.cnt() << " records (total: "
		    << nEvents << ") took " << timer.realTime()
		    << " sec elapsed time and produced "
		    << hits.cnt() << " hits\n";
#if defined(DEBUG) && DEBUG + 0 > 1
	lg.buffer() << "mask\n" << mask << "\nhit vector\n" << hits;
#endif
    }
    if (ierr >= 0)
	ierr = hits.cnt();
    return ierr;
} // ibis::part::doScan

long ibis::part::matchAny(const ibis::qAnyAny& cmp,
			  ibis::bitvector& hits) const {
    if (cmp.getPrefix() == 0 || cmp.getValues().empty()) return -1;
    if (nEvents == 0) return 0;
    ibis::bitvector mask;
    mask.set(1, nEvents);
    return matchAny(cmp, hits, mask);
} // ibis::part::matchAny

/// Perform exact match operation for an AnyAny query.  The bulk of the
/// work is performed as range query.
long ibis::part::matchAny(const ibis::qAnyAny& cmp,
			  const ibis::bitvector& mask,
			  ibis::bitvector& hits) const {
    if (cmp.getPrefix() == 0 || cmp.getValues().empty()) return -1;
    if (nEvents == 0) return 0;

    hits.set(0, mask.size());
    const char* pref = cmp.getPrefix();
    const int len = strlen(pref);
    const std::vector<double>& vals = cmp.getValues();
    columnList::const_iterator it = columns.lower_bound(pref);
    if (vals.size() > 1) { // more than one value
	while (it != columns.end() &&
	       0 == strnicmp((*it).first, pref, len)) {
	    // the column name has the specified prefix
	    ibis::bitvector msk, res; // the mask for this particular column
	    (*it).second->getNullMask(msk);
	    msk &= mask;
	    ibis::qDiscreteRange ex((*it).first, vals);
	    // TODO: some way to decide whether to do this
	    if (hits.cnt() > hits.bytes())
		msk -= hits;
	    doScan(ex, msk, res);
	    if (res.size() == hits.size())
		hits |= res;
	    ++ it;
	}
    }
    else { // a single value to match
	while (it != columns.end() &&
	       0 == strnicmp((*it).first, pref, len)) {
	    // the column name has the specified prefix
	    ibis::bitvector msk, res; // the mask for this particular column
	    (*it).second->getNullMask(msk);
	    msk &= mask;
	    ibis::qContinuousRange ex((*it).first, ibis::qExpr::OP_EQ,
				      vals.back());
	    // hits.bytes() -- represent work of operator-=
	    // hits.cnt() -- additional elements scanned without this
	    // operation
	    // should be worth it in most case!
	    //if (hits.cnt() > hits.bytes())
	    msk -= hits;
	    doScan(ex, msk, res);
	    if (res.size() == hits.size())
		hits |= res;
	    ++ it;
	}
    }
    return hits.cnt();
} // ibis::part::matchAny

// actually compute the min and max of each attribute and write out a new
// TDC file
void ibis::part::computeMinMax() {
    for (columnList::iterator it=columns.begin(); it!=columns.end(); ++it) {
	(*it).second->computeMinMax();
    }
    if (activeDir == 0) return;

    writeTDC(nEvents, columns, activeDir);
    Stat_T tmp;
    if (backupDir != 0 && *backupDir != 0 && UnixStat(backupDir, &tmp) == 0) {
	if ((tmp.st_mode&S_IFDIR) == S_IFDIR) {
	    writeTDC(nEvents, columns, backupDir);
	}
    }
} // ibis::part::computeMinMax

/// May use @c nthr threads to build indices.
/// @sa ibis::part::loadIndex
void ibis::part::buildIndex(int nthr, const char* opt) {
    ibis::horometer timer;
    timer.start();
    if (ibis::gVerbose > 5)
	logMessage("buildIndex",
		   "start to load indexes of this data partition");
    if (nthr > 1) {
	-- nthr; // spawn one less thread than specified
	indexBuilderPool pool(*this, opt);
	std::vector<pthread_t> tid(nthr);
	pthread_attr_t tattr;
	int ierr = pthread_attr_init(&tattr);
	if (ierr == 0) {
#if defined(PTHREAD_SCOPE_SYSTEM)
	    ierr = pthread_attr_setscope(&tattr, PTHREAD_SCOPE_SYSTEM);
	    if (ierr != 0
#if defined(ENOTSUP)
		&& ierr != ENOTSUP
#endif
		) {
		logMessage("buildIndex", "pthread_attr_setscope is unable "
			   "to set system scope (ierr = %d)", ierr);
	    }
#endif
	    for (long i = 0; i < nthr; ++i) {
		ierr = pthread_create(&(tid[i]), &tattr,
				      ibis_part_build_index,
				      (void*)&pool);
		if (0 != ierr) {
		    logWarning("buildIndex", "unable to start the thread # "
			       "%ld to run ibis_part_startTests", i);
		}
	    }
	}
	else {
	    logWarning("buildIndex", "pthread_attr_init failed with %d, "
		       "using default attributes", ierr);
	    for (long i = 0; i < nthr; ++i) {
		ierr = pthread_create(&(tid[i]), 0, ibis_part_build_index,
				      (void*)&pool);
		if (0 != ierr) {
		    logWarning("buildIndex", "unable to start the thread # "
			       "%ld to run ibis_part_startTests", i);
		}
	    }
	}
	(void) ibis_part_build_index((void*)&pool);
	for (int i = 0; i < nthr; ++ i) {
	    void *j;
	    pthread_join(tid[i], &j);
	    if (j != 0) {
		logWarning("buildIndex", "thread # %i returned a "
			   "nonzero code %ld", i,
			   reinterpret_cast<long int>(j));
	    }
	}
    }
    else { // do not spawn any new threads
	for (columnList::iterator it=columns.begin();
	     it!=columns.end();
	     ++it) {
	    if (! ((*it).second->upperBound() >= (*it).second->lowerBound()))
		(*it).second->computeMinMax();
	    (*it).second->loadIndex(opt);
	    (*it).second->unloadIndex();
	}
    }
    if (ibis::gVerbose > 0) {
	timer.stop();
	nthr = (nthr > 0 ? nthr+1 : 1);
	ibis::util::logger lg(0);
	lg.buffer() << "ibis::part[" << name() << "]::buildIndex build "
		    << nColumns() << " index" << (nColumns()>1 ? "es" : "")
		    << " using " << nthr << " thread" << (nthr > 1 ? "s" : "")
		    << " took " << timer.CPUTime() << " CPU seconds and "
		    << timer.realTime() << " elapsed seconds";
    }
} // ibis::part::buildIndex

/// Load the metadata about indexes to memory.  This function iterates
/// through all columns and load the index associated with each one of
/// them.  If an index for a column does not exist, the index is built in
/// memory and written to disk.
///
/// @note An index can not be built correctly if it is too large to fit in
/// memory!
void ibis::part::loadIndex(const char* opt) const {
    if (activeDir == 0) return;

    if (ibis::gVerbose > 5)
	logMessage("loadIndex",
		   "start to load indexes of this data partition");
    for (columnList::const_iterator it=columns.begin(); it!=columns.end();
	 ++it) {
	(*it).second->loadIndex(opt);
    }

    const char* expf = ibis::gParameters()["exportBitmapAsCsr"];
    if (expf != 0 && *expf != 0) {
	// write out the bitmap index in CSR format
	std::vector< ibis::index* > idx;
	columnList::const_iterator it;
	array_t<uint32_t> cnt;
	uint32_t i, j, tot = 0;
	cnt.reserve(nColumns()*12);
	idx.reserve(nColumns());
	for (i = 0, it = columns.begin(); i < nColumns(); ++i, ++it) {
	    idx.push_back(ibis::index::create((*it).second, activeDir));
	    for (j = 0; j < idx[i]->numBitvectors(); ++j) {
		const ibis::bitvector* tmp = idx[i]->getBitvector(j);
		if (tmp) {
		    const uint32_t ct = tmp->cnt();
		    if (ct > 0) {
			cnt.push_back(ct);
			tot += ct;
		    }
		}
	    }
	}

	if (ibis::gVerbose > 1)
	    logMessage("loadIndex", "attempt to write %lu bitmap(s) (%lu) "
		       "to %s", static_cast<long unsigned>(cnt.size()),
		       static_cast<long unsigned>(tot), expf);
	FILE* fptr = fopen(expf, "w"); // write in ASCII text
	if (fptr) { // ready to write out the data
	    fprintf(fptr, "%lu %lu %lu\n0\n",
		    static_cast<long unsigned>(nRows()),
		    static_cast<long unsigned>(cnt.size()),
		    static_cast<long unsigned>(tot));
	    tot = 0;
	    for (i = 0; i < cnt.size(); ++i) {
		tot += cnt[i];
		fprintf(fptr, "%lu\n", static_cast<long unsigned>(tot));
	    }
	    ibis::bitvector::indexSet is;
	    uint32_t nis=0;
	    const ibis::bitvector::word_t *iis=is.indices();
	    for (i = 0; i < idx.size(); ++i) {
		for (j = 0; j < idx[i]->numBitvectors(); ++j) {
		    const ibis::bitvector* tmp = idx[i]->getBitvector(j);
		    if (tmp == 0) continue;
		    is = tmp->firstIndexSet();
		    nis = is.nIndices();
		    while (nis) {
			if (is.isRange()) {
			    for (uint32_t k = iis[0]; k < iis[1]; ++k)
				fprintf(fptr, "%lu\n",
					static_cast<long unsigned>(k));
			}
			else {
			    for (uint32_t k = 0; k < nis; ++k)
				fprintf(fptr, "%lu\n",
					static_cast<long unsigned>(iis[k]));
			}
			++is;
			nis = is.nIndices();
		    } // while (nis)
		} // for (j = 0;
	    } // for (i = 0;
	    fclose(fptr);
	}
	else if (ibis::gVerbose > 0)
	    logMessage("loadIndex", "failed to open file \"%s\" to write "
		       "the bitmaps ... %s", expf,
		       (errno ? strerror(errno) : "no free stdio stream"));

	for (i = 0; i < idx.size(); ++i)
	    delete idx[i];
    }
} // ibis::part::loadIndex

// unload index
void ibis::part::unloadIndex() const {
    if (ibis::gVerbose > 5)
	logMessage("unloadIndex",
		   "start to unload indexes of this data partition");
    for (columnList::const_iterator it=columns.begin(); it!=columns.end();
	 ++it) {
	(*it).second->unloadIndex();
    }
} // ibis::part::unloadIndex

/// @note The indices will be rebuilt next time they are needed.  This
/// function is useful after changing the index specification before
/// rebuilding a set of new indices.
void ibis::part::purgeIndexFiles() const {
    writeLock lock(this, "purgeIndexFiles");
    for (columnList::const_iterator it = columns.begin();
	 it != columns.end();
	 ++ it) {
	(*it).second->purgeIndexFile();
    }
} // ibis::part::purgeIndexFiles

void ibis::part::indexSpec(const char *spec) {
    writeLock lock(this, "indexSpec");
    delete [] idxstr;
    idxstr = ibis::util::strnewdup(spec);
    if (activeDir != 0)
	writeTDC(nEvents, columns, activeDir);
    if (backupDir != 0)
	writeTDC(nEvents, columns, backupDir);
} // ibis::part::indexSpec

// need a read lock to ensure the sanity of the state
ibis::part::TABLE_STATE ibis::part::getState() const {
    readLock lock(this, "getState");
    return state;
} // ibis::part::getState

// perform predefined set of tests and return the number of failures
long ibis::part::selfTest(int nth, const char* pref) const {
    long nerr = 0;
    if (activeDir == 0) return nerr;

    ibis::horometer timer;
    try {
	readLock lock(this, "selfTest"); // need a read lock on the part
	if (ibis::gVerbose > 1) {
	    logMessage("selfTest", "start testing data in %s with option "
		       "%d ... ", activeDir, nth);
	    timer.start();
	}
	columnList::const_iterator it;

	if (columns.size() == 0 || nEvents == 0) {
	    logMessage("selfTest", "empty ibis::part in %s", activeDir);
	    return nerr;
	}

	// test the file sizes based on column::elementSize
	uint32_t sz = 0;
	for (it = columns.begin(); it != columns.end(); ++it) {
	    int elm = (*it).second->elementSize();
	    if (elm > 0) { // the column has fixed element size
		char* fname = (*it).second->dataFileName();
		uint32_t fsize = ibis::util::getFileSize(fname);
		delete [] fname;
		sz = fsize / elm;
		if (sz != nEvents) {
		    ++ nerr;
		    logWarning("selfTest", "column %s has %lu records, "
			       "%lu expected", (*it).first,
			       static_cast<long unsigned>(sz),
			       static_cast<long unsigned>(nEvents));
		}
		else if (ibis::gVerbose > 4) {
		    logMessage("selfTest", "column %s has %lu records as "
			       "expected.", (*it).first,
			       static_cast<long unsigned>(nEvents));
		}
	    }
	    else if (elm < 0) { // a bad element size
		++ nerr;
		logWarning("selfTest", "column %s [tyoe %d] has an "
			   "unsupported type (element size = %d)",
			   (*it).first,
			   static_cast<int>((*it).second->type()),
			   elm);
	    }

	    std::string tmp;
	    if (pref) {
		tmp = pref;
		tmp += ".testIndexSpeed";
	    }
	    else {
		tmp = m_name;
		tmp = ".testIndexSpeed";
	    }
	    if (ibis::gParameters().isTrue(tmp.c_str())) {
		(*it).second->indexSpeedTest();
	    }
	}
	if (nth <= 0 || nerr > 0)
	    return nerr;

	bool longtest = false;
	{
	    std::string ltest;
	    if (pref) {
		ltest = pref;
		ltest += ".longTests";
	    }
	    else {
		ltest = name();
		ltest += ".longTests";
	    }
	    longtest = ibis::gParameters().isTrue(ltest.c_str());
	}
	if (nth > 1) {
	    // select some attributes for further testing -- do part of the
	    // work in a new thread
	    -- nth;
	    if (nth > 100)
		nth = 100;
	    std::vector<pthread_t> tid(nth);
	    if (tid.empty()) {
		logWarning("selfTest", "failed to allocate array "
			   "pthread_t[%d], will run only one set of tests",
			   nth);
		++ nerr;
		quickTest(pref, &nerr);
		return nerr;
	    }

	    thrArg arg;
	    arg.et = this;
	    arg.pref = pref;
	    arg.nerrors = &nerr;
	    pthread_attr_t tattr;
	    int ierr = pthread_attr_init(&tattr);
	    if (ierr == 0) {
#if defined(PTHREAD_SCOPE_SYSTEM)
		ierr = pthread_attr_setscope(&tattr, PTHREAD_SCOPE_SYSTEM);
		if (ierr != 0
#if defined(ENOTSUP)
		    && ierr != ENOTSUP
#endif
		    ) {
		    logMessage("selfTest", "pthread_attr_setscope is unable "
			       "to set system scope (ierr = %d)", ierr);
		}
#endif
		for (int i = 0; i < nth; ++i) {
		    ierr = pthread_create(&(tid[i]), &tattr,
					  ibis_part_startTests,
					  (void*)&arg);
		    if (0 != ierr) {
			logWarning("selfTest", "unable to start the thread "
				   "# %d to run ibis_part_startTests", i);
		    }
		}
	    }
	    else {
		logWarning("selfTest", "pthread_attr_init failed with %d, "
			   "using default attributes", ierr);
		for (int i = 0; i < nth; ++i) {
		    ierr = pthread_create(&(tid[i]), 0,
					  ibis_part_startTests,
					  (void*)&arg);
		    if (0 != ierr) {
			logWarning("selfTest", "unable to start the thread "
				   "# %d to run ibis_part_startTests", i);
		    }
		}
	    }

	    // handle part of work using this thread
	    if (nEvents < 1048576 || longtest)
		queryTest(pref, &nerr);
	    else
		quickTest(pref, &nerr);

	    for (int i = 0; i < nth; ++i) { // wait for the other threads
		void* j;
		pthread_join(tid[i], &j);
		if (j != 0)
		    logWarning("selfTest", "thread # %d returned "
			       "a nonzero code %ld", i,
			       reinterpret_cast<long int>(j));
	    }
	    ierr = pthread_attr_destroy(&tattr);
	}
	else if (nth > 0) { // try some queries using this thread
	    if (nEvents < 1048576 || longtest)
		queryTest(pref, &nerr);
	    else
		quickTest(pref, &nerr);
	}
    }
    catch (const std::exception& e) {
	ibis::util::logMessage("Warning", "ibis::part::selfTest() received "
			       "the following a std::exception\n%s",
			       e.what());
	++ nerr;
    }
    catch (const char* s) {
	ibis::util::logMessage("Warning", "ibis::part::selfTest() received "
			       "the following string exception\n%s", s);
	++ nerr;
    }
    catch (...) {
	ibis::util::logMessage("Warning", "ibis::part::selfTest() received "
			       "an unexpected exception");
	++ nerr;
    }
    if (nerr) {
	logWarning("selfTest", "encountered %d error%s", nerr,
		   (nerr>1?"s":""));
    }
    else if (ibis::gVerbose > 1) {
	timer.stop();
	logMessage("selfTest", "completed successfully using %g sec(CPU) "
		   "and %g sec(elapsed)", timer.CPUTime(),
		   timer.realTime());
    }

    return nerr;
} // ibis::part::selfTest

// randomly select a column and perform about a set of test recursively
void ibis::part::queryTest(const char* pref, long* nerrors) const {
    if (columns.empty() || nEvents == 0) return;

    // select a colum to perform the test on
    int i = (static_cast<int>(ibis::util::rand() * columns.size()) +
	     ibis::fileManager::instance().iBeat()) % columns.size();
    columnList::const_iterator it = columns.begin();
    while (i) {++it; --i;};
    for (i = 0; static_cast<unsigned>(i) < columns.size() &&
	     (*it).second->type() == ibis::TEXT; ++ i) {
	// skip over string attributes
	++ it;
	if (it == columns.end()) // wrap around
	    it = columns.begin();
    }
    if (static_cast<unsigned>(i) >= columns.size()) {
	logWarning("queryTest",
		   "unable to find a non-string attribute for testing");
	return;
    }

    // select the range to test
    double lower = (*it).second->lowerBound();
    double upper = (*it).second->upperBound();
    if (! (lower < upper)) {
	// actually compute the minimum and maximum values
	(*it).second->computeMinMax();
	lower = (*it).second->lowerBound();
	upper = (*it).second->upperBound();
    }
    if (! (lower < upper)) {
#if defined(_WIN32) && !defined(__CYGWIN__)
	if (_finite(lower))
#else
	    if (finite(lower))
#endif
		upper = ibis::util::compactValue(lower, DBL_MAX);
#if defined(_WIN32) && !defined(__CYGWIN__)
	    else if (_finite(upper))
#else
		else if (finite(upper))
#endif
		    lower = ibis::util::compactValue(-DBL_MAX, upper);

	if (! (lower < upper)) { // force min/max to be 0/1
	    lower = 0.0;
	    upper = 1.0;
	}
    }

    std::string random;
    if (pref) {
	random = pref;
	random += ".randomTests";
    }
    else {
	random = "randomTests";
    }
    if (ibis::gParameters().isTrue(random.c_str())) {
	unsigned tmp1 = time(0);
	unsigned tmp2 = rand();
	double range = (*it).second->upperBound()
	    - (*it).second->lowerBound();
	lower = range * ((tmp1 % 1024) / 1024.0);
	upper = range *((tmp2 % 1024) / 1024.0);
	if (fabs(lower - upper)*256 < range) {
	    lower = (*it).second->lowerBound();
	    upper = (*it).second->upperBound();
	}
	else if (lower < upper) {
	    lower += (*it).second->lowerBound();
	    upper += (*it).second->lowerBound();
	}
	else {
	    range = lower;
	    lower = upper + (*it).second->lowerBound();
	    upper = range + (*it).second->lowerBound();
	}
	ibis::TYPE_T type = (*it).second->type();
	if (type != ibis::FLOAT && type != ibis::DOUBLE) {
	    // remove the fractional part for consistent printing
	    lower = std::floor(lower);
	    upper = std::ceil(upper);
	}
    }

    ibis::horometer timer;
    if (ibis::gVerbose > 2)
	timer.start();
    // do the test
    recursiveQuery(pref, (*it).second, lower, upper, nerrors);
    if (ibis::gVerbose > 2) {
	timer.stop();
	logMessage("queryTest", "tests on %s took %g sec(CPU) and %g "
		   "sec(elapsed)", (*it).first, timer.CPUTime(),
		   timer.realTime());
    }
} // ibis::part::queryTest

// randomly select a column from the current list and perform a dozen tests
// on the column
void ibis::part::quickTest(const char* pref, long* nerrors) const {
    if (columns.empty() || nEvents == 0) return;

    char clause[MAX_LINE];
    // select a colum to perform the test on
    columnList::const_iterator it = columns.begin();
    {
	int i = (static_cast<int>(ibis::util::rand() * columns.size()) +
		 ibis::fileManager::instance().iBeat()) % columns.size();
	while (i) {++it; --i;};
	for (i = 0; static_cast<unsigned>(i) < columns.size() &&
		 (*it).second->type() == ibis::TEXT; ++ i) {
	    // skip over string attributes
	    ++ it;
	    if (it == columns.end()) // wrap around
		it = columns.begin();
	}
	if (static_cast<unsigned>(i) >= columns.size()) {
	    logWarning("quickTest",
		       "unable to find a non-string attribute for testing");
	    return;
	}
    }
    ibis::column* att = (*it).second;
    if (it != columns.begin()) {
	-- it;
	sprintf(clause, "%s %s", (*it).first, att->name());
    }
    else if (columns.size() > 1) {
	it = columns.end();
	-- it;
	sprintf(clause, "%s %s", (*it).first, att->name());
    }
    else {
	strcpy(clause, att->name());
    }

    // select the range to test
    double lower = att->lowerBound();
    double upper = att->upperBound();
    if (! (lower < upper)) {
	// actually compute the minimum and maximum values
	att->computeMinMax();
	lower = att->lowerBound();
	upper = att->upperBound();
    }
    if (! (lower < upper)) {
#if defined(_WIN32) && !defined(__CYGWIN__)
	if (_finite(lower))
#else
	    if (finite(lower))
#endif
		upper = ibis::util::compactValue(lower, DBL_MAX);
#if defined(_WIN32) && !defined(__CYGWIN__)
	    else if (_finite(upper))
#else
		else if (finite(upper))
#endif
		    lower = ibis::util::compactValue(-DBL_MAX, upper);

	if (! (lower < upper)) { // force min/max to 0/1
	    lower = 0.0;
	    upper = 1.0;
	}
    }

    std::string random;
    if (pref) {
	random = pref;
	random += ".randomTests";
    }
    else {
	random = "randomTests";
    }
    if (ibis::gParameters().isTrue(random.c_str())) {
	unsigned tmp1 = time(0);
	unsigned tmp2 = rand();
	double range = upper - lower;
	lower += range * ((tmp1 % 1024) / 1024.0);
	upper -= range *((tmp2 % 1024) / 1024.0);
	if (fabs(lower - upper)*512 < range) {
	    // very small difference, use the whole range
	    lower -= range * ((tmp1 % 1024) / 1024.0);
	    upper += range *((tmp2 % 1024) / 1024.0);
	}
	else if (lower > upper) { // lower bound is smaller
	    range = lower;
	    lower = upper;
	    upper = range;
	}
	ibis::TYPE_T type = att->type();
	if (type != ibis::FLOAT && type == ibis::DOUBLE) {
	    // remove the fractional part for consistent printing
	    lower = std::floor(lower);
	    upper = std::ceil(upper);
	}
    }

    ibis::horometer timer;
    if (ibis::gVerbose > 2)
	timer.start();
    // do the test
    const char* str=0;
    uint32_t total=0;
    query qtmp("[:]", this, pref);
    qtmp.setSelectClause(clause);

    sprintf(clause, "%s < %g", att->name(), lower);
    qtmp.setWhereClause(clause);
    long ierr = qtmp.evaluate();
    if (ierr >= 0)
	total = qtmp.getNumHits();
    else
	++ (*nerrors);
    str = qtmp.getLastError();
    if (str != 0 && *str != static_cast<char>(0)) {
	LOGGER(0)
	    << "Warning -- ibis::part::quickTest last error on query "
	    << "\"" << clause << "\" is \n" << str;
	qtmp.clearErrorMessage();
	++(*nerrors);
    }

    sprintf(clause, "%s >= %g", att->name(), upper);
    qtmp.setWhereClause(clause);
    ierr = qtmp.evaluate();
    if (ierr >= 0) {
	total += qtmp.getNumHits();
	if (ibis::gVerbose > 2) { // use sequential scan to verify counts
	    ibis::bitvector tmp;
	    ierr = qtmp.sequentialScan(tmp);
	    if (ierr >= 0) {
		tmp ^= *(qtmp.getHitVector());
		if (tmp.cnt() > 0) {
		    ++ (*nerrors);
		    LOGGER(0)
			<< "Warning -- the sequential scan for "
			<< clause << " produced "
			<< tmp.cnt() << " different result"
			<< (tmp.cnt() > 1 ? "s" : "");
		}
	    }
	    else {
		++ (*nerrors);
	    }
	}
    }
    else {
	++ (*nerrors);
    }
    str = qtmp.getLastError();
    if (str != 0 && *str != static_cast<char>(0)) {
	LOGGER(0)
	    << "Warning -- ibis::part::quickTest last error on query "
	    << "\"" << clause << "\" is \n" << str;
	qtmp.clearErrorMessage();
	++(*nerrors);
    }

    double tgt = lower + 0.01 * (upper - lower);
    double b1 = 0.5*(lower + upper);
    double b2 = upper;
    while (b1 > tgt) {
	sprintf(clause, "%g <= %s < %g", b1, att->name(), b2);
	qtmp.setWhereClause(clause);
	ierr = qtmp.evaluate();
	if (ierr >= 0) {
	    total += qtmp.getNumHits();
	    if (ibis::gVerbose > 2) { // use sequential scan to verify counts
		ibis::bitvector tmp;
		ierr = qtmp.sequentialScan(tmp);
		if (ierr >= 0) {
		    tmp ^= *(qtmp.getHitVector());
		    if (tmp.cnt() > 0) {
			++ (*nerrors);
			LOGGER(0)
			    << "Warning -- the sequential scan for "
			    << clause << " produced "
			    << tmp.cnt() << " different result"
			    << (tmp.cnt() > 1 ? "s" : "");
		    }
		}
		else {
		    ++ (*nerrors);
		}
	    }
	}
	else {
	    ++ (*nerrors);
	}
	str = qtmp.getLastError();
	if (str != 0 && *str != static_cast<char>(0)) {
	    logWarning("quickTest",  "last error on query \"%s\" "
		       "is \n%s", clause, str);
	    qtmp.clearErrorMessage();
	    ++(*nerrors);
	}
	b2 = b1;
	b1 = 0.5*(lower + b1);
    }

    sprintf(clause, "%g <= %s < %g", lower, att->name(), b2);
    qtmp.setWhereClause(clause);
    ierr = qtmp.evaluate();
    if (ierr >= 0)
	total += qtmp.getNumHits();
    else
	++ (*nerrors);
    str = qtmp.getLastError();
    if (str != 0 && *str != static_cast<char>(0)) {
	logWarning("quickTest",  "last error on query \"%s\" "
		   "is \n%s", clause, str);
	qtmp.clearErrorMessage();
	++(*nerrors);
    }

    { // a block to limit the scope of mask
	ibis::bitvector mask;
	att->getNullMask(mask);
	if (total != mask.cnt()) {
	    ++(*nerrors);
	    logWarning("quickTest", "the total number of values for %s is "
		       "expected to be %lu but is actually %lu", att->name(),
		       static_cast<long unsigned>(mask.cnt()),
		       static_cast<long unsigned>(total));
	}
    }

    { // use sequential scan to verify the hit list
	ibis::bitvector seqhits;
	ierr = qtmp.sequentialScan(seqhits);
	if (ierr < 0) {
	    ++(*nerrors);
	    logWarning("quickTest", "sequential scan on query \"%s\" failed",
		       clause);
	}
	else {
	    seqhits ^= *(qtmp.getHitVector());
	    if (seqhits.cnt() != 0) {
		++(*nerrors);
		logWarning("quickTest", "sequential scan on query \"%s\" "
			   "produced %lu different hits", clause,
			   static_cast<long unsigned>(seqhits.cnt()));
		if (ibis::gVerbose > 2) {
		    uint32_t maxcnt = (ibis::gVerbose > 30 ? nRows()
				       : (1U << ibis::gVerbose));
		    if (maxcnt > seqhits.cnt())
			maxcnt = seqhits.cnt();
		    uint32_t cnt = 0;
		    ibis::bitvector::indexSet is = seqhits.firstIndexSet();
		    ibis::util::logger lg;
		    lg.buffer() << "the locations of the difference\n";
		    while (is.nIndices() && cnt < maxcnt) {
			const ibis::bitvector::word_t *ii = is.indices();
			if (is.isRange()) {
			    lg.buffer() << *ii << " -- " << ii[1];
			}
			else {
			    for (uint32_t i0=0; i0 < is.nIndices(); ++i0)
				lg.buffer() << ii[i0] << " ";
			}
			cnt += is.nIndices();
			lg.buffer() << "\n";
			++ is;
		    }
		    if (cnt < seqhits.cnt())
			lg.buffer() << "... (" << seqhits.cnt() - cnt
				    << " rows skipped\n";
		}
	    }
	    else if (ibis::gVerbose > 3) {
		logMessage("quickTest",
			   "sequential scan produced the same hits");
	    }
	}
    }
    // test RID query -- limit to no more than 1024 RIDs to save time
    ibis::RIDSet* rid1 = qtmp.getRIDs();
    if (rid1 == 0 || rid1->empty()) {
	delete rid1;
	if (ibis::gVerbose > 1) {
	    timer.stop();
	    logMessage("quickTest", "tests on %s took %g sec(CPU) and %g "
		       "sec(elapsed)", att->name(), timer.CPUTime(),
		       timer.realTime());
	}
	return;
    }
#if defined(DEBUG)
    {
	ibis::util::logger lg(ibis::gVerbose);
	const char* str = qtmp.getWhereClause();
	lg.buffer() << "DEBUG: query[" << qtmp.id() << ", "
		    << (str?str:"<NULL>") << "]::getRIDs returned "
		    << rid1->size() << "\n";
#if DEBUG > 2
	for (ibis::RIDSet::const_iterator it1 = rid1->begin();
	     it1 != rid1->end(); ++it1)
	    lg.buffer() << (*it1) << "\n";
#endif
    }
#endif
    if (rid1->size() > 2048)
	rid1->resize(2047 & rid1->size());
    std::sort(rid1->begin(), rid1->end());
#if defined(DEBUG)
    {
	ibis::util::logger lg(ibis::gVerbose);
	lg.buffer() << "rid1 after query[" << qtmp.id()
		    << "]::evaluate contains " << rid1->size() << "\n";
#if DEBUG > 2
	for (ibis::RIDSet::const_iterator it1 = rid1->begin();
	     it1 != rid1->end(); ++it1)
	    lg.buffer() << (*it1) << "\n";
#endif
    }
#endif
    ibis::RIDSet* rid2 = new ibis::RIDSet();
    rid2->deepCopy(*rid1); // setRIDs removes the underlying file for rid1
    delete rid1;
    rid1 = rid2;
    qtmp.setRIDs(*rid1);
    ierr = qtmp.evaluate();
    if (ierr >= 0) {
	rid2 = qtmp.getRIDs();
	std::sort(rid2->begin(), rid2->end());
	if (rid1->size() == rid2->size()) {
	    ibis::util::logger lg(ibis::gVerbose);
	    uint32_t i, cnt=0;
	    for (i=0; i<rid1->size(); ++i) {
		if ((*rid1)[i].value != (*rid2)[i].value) {
		    ++cnt;
		    lg.buffer() << i << "th RID " << (*rid1)[i]
			      << " != " << (*rid2)[i] << "\n";
		}
	    }
	    if (cnt > 0) {
		lg.buffer() << "Warning -- query[" << qtmp.id() << "] " << cnt
			  << " mismatches out of a total of "
			  << rid1->size() << "\n";
		++(*nerrors);
	    }
	    else if (ibis::gVerbose > 4) {
		lg.buffer() << "RID query returned the expected RIDs"
			  << "\n";
	    }
	}
	else {
	    ibis::util::logger lg(ibis::gVerbose);
	    lg.buffer() << "Warning -- query[" << qtmp.id() << "] sent "
		      << rid1->size() << " RIDs, got back "
		      << rid2->size() << "\n";
	    uint32_t i=0, cnt;
	    cnt = (rid1->size() < rid2->size()) ? rid1->size() :
		rid2->size();
	    while (i < cnt) {
		lg.buffer() << (*rid1)[i] << " >>> " << (*rid2)[i] << "\n";
		++i;
	    }
	    if (rid1->size() < rid2->size()) {
		while (i < rid2->size()) {
		    lg.buffer() << "??? >>> " << (*rid2)[i] << "\n";
		    ++i;
		}
	    }
	    else {
		while (i < rid1->size()) {
		    lg.buffer() << (*rid1)[i] << " >>> ???\n";
		    ++i;
		}
	    }
	    lg.buffer() << "\n";
	    ++(*nerrors);
	}
	delete rid2;
    }
    else {
	++ (*nerrors);
    }
    delete rid1;

    if (ibis::gVerbose > 2) {
	timer.stop();
	logMessage("quickTest", "tests on %s took %g sec(CPU) and %g "
		   "sec(elapsed)", att->name(), timer.CPUTime(),
		   timer.realTime());
    }
} // ibis::part::quickTest

// issues query and then subdivided the range into three to check the total
// hits of the three sub queries matches the hits of the single query allow
// a maximum of 6 levels of recursion, no more than 80 queries
uint32_t ibis::part::recursiveQuery(const char* pref, const column* att,
				    double low, double high,
				    long *nerrors) const {
    uint32_t cnt0, cnt1, cnt2, cnt3;
    double mid1, mid2;
    {// use a block to limit the scope of query object
	char predicate[MAX_LINE];
	sprintf(predicate, "%g <= %s < %g", low, att->name(), high);
	query qtmp("[:]", this, pref);
	qtmp.setSelectClause(att->name());
	qtmp.setWhereClause(predicate);
	const char *str = qtmp.getLastError();
	if (str != 0 && *str != static_cast<char>(0)) {
	    ibis::util::logger lg;
	    lg.buffer() << "Warning -- ibis::part::queryTest last error on "
			<< "query \"" << predicate << "\" is \n" << str;
	    qtmp.clearErrorMessage();
	    ++(*nerrors);
	}

	if (ibis::gVerbose > 1)
	    qtmp.logMessage("queryTest", "selectivity = %g", (high-low) /
			    (att->upperBound() - att->lowerBound()));

	// compute the number of hits
	qtmp.estimate(); // to provide some timing data
	int ierr = qtmp.evaluate();
	if (ierr >= 0) {
	    cnt0 = qtmp.getNumHits();
	}
	else {
	    cnt0 = 0;
	    ++ (*nerrors);
	}
	str = qtmp.getLastError();
	if (str != 0 && *str != static_cast<char>(0)) {
	    ibis::util::logger lg;
	    lg.buffer() << "Warning -- ibis::part::queryTest last error on "
			<< "query \"" << predicate << "\" is \n" << str;
	    qtmp.clearErrorMessage();
	    ++(*nerrors);
	}

	LOGGER(5) << "ibis::part::queryTest(" << att->name() << ") found "
		  << cnt0 << " hit" << (cnt0<2?"":"s") << " in ["
		  << low << ", " << high << ")";

	{ // use sequential scan to verify the hit list
	    ibis::bitvector seqhits;
	    ierr = qtmp.sequentialScan(seqhits);
	    if (ierr < 0) {
		++(*nerrors);
		logWarning("queryTest", "sequential scan failed");
	    }
	    else if (seqhits.cnt() != cnt0) {
		++(*nerrors);
		logWarning("queryTest", "sequential scan on \"%s\" produced "
			   "%lu, but evaluate produced %lu", predicate,
			   static_cast<long unsigned>(seqhits.cnt()),
			   static_cast<long unsigned>(cnt0));
	    }
	    else {
		seqhits ^= *(qtmp.getHitVector());
		if (seqhits.cnt() > 0) {
		    ++(*nerrors);
		    logWarning("queryTest", "sequential scan on \"%s\" "
			       "produced %lu different result%s", predicate,
			       static_cast<long unsigned>(seqhits.cnt()),
			       (seqhits.cnt() > 1 ? "s" : ""));
#if defined(DEBUG)
		    seqhits.compress();
		    LOGGER(0)
			<< "DEBUG: queryTest -- " << seqhits;
#endif
		}
		else if (ibis::gVerbose > 3) {
		    logMessage("queryTest",
			       "sequential scan produced the same hits");
		}
	    }
	}

	if (low == att->lowerBound() && high == att->upperBound()) {
	    sprintf(predicate, "%s < %g", att->name(), low);
	    qtmp.setWhereClause(predicate);
	    ierr = qtmp.evaluate();
	    if (ierr >= 0) {
		cnt1 = qtmp.getNumHits();
	    }
	    else {
		cnt1 = 0;
		++ (*nerrors);
	    }
	    sprintf(predicate, "%s >= %g", att->name(), high);
	    qtmp.setWhereClause(predicate);
	    ierr = qtmp.evaluate();
	    if (ierr >= 0) {
		cnt2 = qtmp.getNumHits();
	    }
	    else {
		cnt2 = 0;
		++ (*nerrors);
	    }
	    if (cnt0+cnt1+cnt2 != nEvents) {
		logWarning("queryTest", "The total of %lu %s entries "
			   "(%lu |%g| %lu |%g| %lu) is different from the "
			   "expected %lu",
			   static_cast<long unsigned>(cnt0+cnt1+cnt2),
			   att->name(),
			   static_cast<long unsigned>(cnt1), low,
			   static_cast<long unsigned>(cnt0), high,
			   static_cast<long unsigned>(cnt2),
			   static_cast<long unsigned>(nEvents));
		++(*nerrors);
	    }
	    else if (ibis::gVerbose > 3) {
		logMessage("queryTest", "The total of %lu %s entries "
			   "(%lu |%g| %lu |%g| %lu) is the same as the "
			   "expected %lu",
			   static_cast<long unsigned>(cnt0+cnt1+cnt2),
			   att->name(),
			   static_cast<long unsigned>(cnt1), low,
			   static_cast<long unsigned>(cnt0), high,
			   static_cast<long unsigned>(cnt2),
			   static_cast<long unsigned>(nEvents));
	    }
	}
    }

    mid1 = high - low;
    double range = att->upperBound() - att->lowerBound();
    if ((mid1*64 > range) && (cnt0*256 > nEvents)) {
	mid1 = 0.25 * (low*3 + high);
	mid2 = 0.125 * (low + high*7);
	if (att->type() != ibis::FLOAT &&
	    att->type() != ibis::DOUBLE) {
	    // get rid of the fractional parts for consistent printing --
	    // query processing automatically truncates floating-point
	    // values to integers
	    mid1 = std::ceil(mid1);
	    mid2 = std::floor(mid2);
	}
	if (mid1 < mid2) {
	    cnt1 = recursiveQuery(pref, att, low, mid1, nerrors);
	    cnt2 = recursiveQuery(pref, att, mid1, mid2, nerrors);
	    cnt3 = recursiveQuery(pref, att, mid2, high, nerrors);
	    if (cnt0 != (cnt1 + cnt2 + cnt3)) {
		logWarning("queryTest", "The total of %lu %s rows "
			   "[%g| %lu |%g| %lu |%g| %lu |%g) is different "
			   "from the expected value %lu",
			   static_cast<long unsigned>(cnt1+cnt2+cnt3),
			   att->name(), low, static_cast<long unsigned>(cnt1),
			   mid1, static_cast<long unsigned>(cnt2), mid2,
			   static_cast<long unsigned>(cnt3), high,
			   static_cast<long unsigned>(cnt0));
		++(*nerrors);
	    }
	    else if (ibis::gVerbose > 3) {
		logMessage("queryTest", "The total of %lu %s rows "
			   "[%g| %lu |%g| %lu |%g| %lu |%g) is the same as "
			   "the expected value %lu",
			   static_cast<long unsigned>(cnt1+cnt2+cnt3),
			   att->name(), low, static_cast<long unsigned>(cnt1),
			   mid1, static_cast<long unsigned>(cnt2), mid2,
			   static_cast<long unsigned>(cnt3), high,
			   static_cast<long unsigned>(cnt0));
	    }
	}
    }
    return cnt0;
} // ibis::part::recursiveQuery

// three error logging functions
void ibis::part::logError(const char* event, const char* fmt, ...) const {
#if (defined(HAVE_VPRINTF) || defined(_WIN32)) && ! defined(DISABLE_VPRINTF)
    char* s = new char[strlen(fmt)+MAX_LINE];
    if (s != 0) {
	va_list args;
	va_start(args, fmt);
	vsprintf(s, fmt, args);
	va_end(args);

	{
	    ibis::util::logger lg(ibis::gVerbose + 2);
	    lg.buffer() << " Error *** part[" << (m_name?m_name:"") << "]::"
			<< event << " -- " << s;
	    if (errno != 0)
		lg.buffer() << " ... " << strerror(errno);
	}
	throw s;
    }
    else {
#endif
	{
	    ibis::util::logger lg(ibis::gVerbose + 2);
	    lg.buffer() << " Error *** part[" << (m_name?m_name:"") << "]::"
			<< event << " == " << fmt << " ...";
	    if (errno != 0)
		lg.buffer() << " ... " << strerror(errno);
	}
	throw fmt;
#if (defined(HAVE_VPRINTF) || defined(_WIN32)) && ! defined(DISABLE_VPRINTF)
    }
#endif
} // ibis::part::logError

void ibis::part::logWarning(const char* event,
			    const char* fmt, ...) const {
    if (ibis::gVerbose < 0)
	return;
    char tstr[28];
    ibis::util::getLocalTime(tstr);
    FILE* fptr = ibis::util::getLogFile();

    ibis::util::ioLock lock;
    fprintf(fptr, "%s\nWarning -- part[%s]::%s -- ", tstr,
	    (m_name?m_name:""), event);

#if (defined(HAVE_VPRINTF) || defined(_WIN32)) && ! defined(DISABLE_VPRINTF)
    va_list args;
    va_start(args, fmt);
    vfprintf(fptr, fmt, args);
    va_end(args);
#else
    fprintf(fptr, "%s ...", fmt);
#endif
    if (errno != 0) {
	if (errno != ENOENT)
	    fprintf(fptr, " ... %s", strerror(errno));
	errno = 0;
    }
    fprintf(fptr, "\n");
    fflush(fptr);
} // ibis::part::logWarning

void ibis::part::logMessage(const char* event,
			    const char* fmt, ...) const {
    FILE* fptr = ibis::util::getLogFile();
    ibis::util::ioLock lock;
#if defined(TIMED_LOG)
    char tstr[28];
    ibis::util::getLocalTime(tstr);
    fprintf(fptr, "%s   ", tstr);
#endif
    fprintf(fptr, "part[%s]::%s -- ", (m_name?m_name:"?"), event);
#if (defined(HAVE_VPRINTF) || defined(_WIN32)) && ! defined(DISABLE_VPRINTF)
    va_list args;
    va_start(args, fmt);
    vfprintf(fptr, fmt, args);
    va_end(args);
#else
    fprintf(fptr, "%s ...", fmt);
#endif
    fprintf(fptr, "\n");
    fflush(fptr);
} // ibis::part::logMessage

// The function that performs the actual comparison for range queries.  The
// size of array may either match the number of bits in @c mask or the
// number of set bits in @c mask.  This allows one to either use the whole
// array or the only the elements need for this operation.  In either case,
// only mask.cnt() elements of array are checked but position of the bits
// that need to be set in the output bitvector @c hits have to be handled
// differently.
template <typename T>
long ibis::part::doCompare(const array_t<T>& array,
			   const ibis::bitvector& mask,
			   ibis::bitvector& hits,
			   const ibis::qRange& cmp) const {
    ibis::horometer timer;
    if (ibis::gVerbose > 1) timer.start(); // start the timer

    long ierr = 0;
    uint32_t i=0, j=0;
    const bool uncomp = ((mask.size() >> 8) < mask.cnt());
    if (uncomp) { // use uncompressed hits internally
	hits.set(0, mask.size());
	hits.decompress();
    }
    else {
	hits.clear();
	hits.reserve(mask.size(), mask.cnt());
    }

    ibis::bitvector::indexSet idx = mask.firstIndexSet();
    if (array.size() == mask.size()) { // full array available
	while (idx.nIndices() > 0) { // the outer loop
	    const ibis::bitvector::word_t *ii = idx.indices();
	    if (idx.isRange()) {
		for (j = *ii; j < ii[1]; ++j) {
		    if (cmp.inRange(array[j])) {
			hits.setBit(j, 1);
			++ ierr;
#if defined(DEBUG) && DEBUG + 0 > 1
			LOGGER(0)
			    << "DEBUG: doCompare" << array[j] << " is in "
			    << cmp;
#endif
		    }
		}
	    }
	    else {
		for (i = 0; i < idx.nIndices(); ++i) {
		    j = ii[i];
		    if (cmp.inRange(array[j])) {
			hits.setBit(j, 1);
			++ ierr;
#if defined(DEBUG) && DEBUG + 0 > 1
			LOGGER(0)
			    << "DEBUG: doCompare " << array[j] << " is in "
			    << cmp;
#endif
		    }
		}
	    }

	    ++idx; // next set of selected entries
	}
    }
    else if (array.size() == mask.cnt()) { // packed array available
	while (idx.nIndices() > 0) {
	    const ibis::bitvector::word_t *ii = idx.indices();
	    if (idx.isRange()) {
		for (unsigned k = *ii; k < ii[1]; ++ k) {
		    if (cmp.inRange(array[j++])) {
			hits.setBit(k, 1);
			++ ierr;
#if defined(DEBUG) && DEBUG + 0 > 1
			LOGGER(0)
			    << "DEBUG: doCompare" << array[j] << " is in "
			    << cmp << ", setting position " << k
			    << " of hit vector to 1";
#endif
		    }
		}
	    }
	    else {
		for (uint32_t k = 0; k < idx.nIndices(); ++ k) {
		    if (cmp.inRange(array[j++])) {
			hits.setBit(ii[k], 1);
			++ ierr;
#if defined(DEBUG) && DEBUG + 0 > 1
			LOGGER(0)
			    << "DEBUG: doCompare " << array[j] << " is in "
			    << cmp << ", setting position " << ii[k]
			    << " of hit vector to 1";
#endif
		    }
		}
	    }
	    ++ idx;
	}
    }
    else {
	logWarning("doCompare", "the input data array size (%lu) has to be "
		   "either %lu or %lu",
		   static_cast<long unsigned>(array.size()),
		   static_cast<long unsigned>(mask.size()),
		   static_cast<long unsigned>(mask.cnt()));
	ierr = -6;
    }

    if (uncomp)
	hits.compress();
    else if (hits.size() < nEvents)
	hits.adjustSize(0, nEvents);

    if (ibis::gVerbose > 1) {
	timer.stop();
	ibis::util::logger lg;
	lg.buffer() << "ibis::part[" << (m_name ? m_name : "?")
		    << "]::doCompare -- performing comparison "
		    << cmp << " on " << mask.cnt() << ' ' << typeid(T).name()
		    << "s from an array of "
		    << array.size() << " elements took "
		    << timer.realTime() << " sec elapsed time and produced "
		    << hits.cnt() << " hits" << "\n";
#if defined(DEBUG) && DEBUG + 0 > 1
	lg.buffer() << "mask\n" << mask << "\nhit vector\n" << hits;
#endif
    }
    return ierr;
} // ibis::part::doCompare

template <typename T>
long ibis::part::doCompare(const char* file,
			   const ibis::bitvector& mask,
			   ibis::bitvector& hits,
			   const ibis::qRange& cmp) const {
    ibis::horometer timer;
    if (ibis::gVerbose > 1)
	timer.start(); // start the timer

    int fdes = UnixOpen(file, OPEN_READONLY);
    if (fdes < 0) {
	logWarning("doCompare", "unable to open file \"%s\"", file);
	hits.set(0, mask.size());
	return -1;
    }
#if defined(_WIN32) && defined(_MSC_VER)
    (void)_setmode(fdes, _O_BINARY);
#endif

    const unsigned elem = sizeof(T);
    // attempt to allocate a decent size buffer for operations
    char *cbuf = 0;
    uint32_t nbuf = ibis::util::getBuffer(cbuf) / elem;
    T *buf = reinterpret_cast<T*>(cbuf);

    uint32_t i=0, j=0;
    long diff, ierr=0;
    const ibis::bitvector::word_t *ii;
    const bool uncomp = ((mask.size() >> 8) < mask.cnt());
    if (uncomp) { // use uncompressed hits internally
	hits.set(0, mask.size());
	hits.decompress();
    }
    else {
	hits.clear();
	hits.reserve(mask.size(), mask.cnt());
    }

    ibis::bitvector::indexSet idx = mask.firstIndexSet();

    if (buf) { // has a good size buffer to read data into
	while (idx.nIndices() > 0) { // the outer loop
	    ii = idx.indices();
	    if (idx.isRange()) {
		diff = *ii * elem;
		ierr = UnixSeek(fdes, diff, SEEK_SET);
		if (ierr != diff) {
		    LOGGER(1) << "ibis::part::doCompare(" << file
			      << ") failed to seek to " << diff;
		    UnixClose(fdes);
		    hits.clear();
		    return -2;
		}
		ibis::fileManager::instance().recordPages
		    (diff, elem*ii[1]);
		j = ii[1];
		for (i = *ii; i < ii[1]; i += diff) {
		    diff = nbuf;
		    if (i+diff > ii[1])
			diff = ii[1] - i;
		    ierr = UnixRead(fdes, buf, elem*diff) / elem;
		    if (diff == ierr && ierr >= 0) {
			j = ierr;
		    }
		    else {
			j = 0;
			logWarning("doCompare", "expected to read %ld "
				   "integers from \"%s\" but got only %ld",
				   diff, file, ierr);
		    }
		    for (uint32_t k = 0; k < j; ++k) {
			if (cmp.inRange(buf[k])) {
			    hits.setBit(i+k, 1);
			}
		    }
		}
	    }
	    else if (idx.nIndices() > 1) {
		j = idx.nIndices() - 1;
		diff = ii[j] - *ii + 1;
		if (static_cast<uint32_t>(diff) < nbuf) {
		    ierr = UnixSeek(fdes, *ii * elem, SEEK_SET);
		    if (ierr != *ii * elem) {
			LOGGER(1) << "Warning -- ibis::part["
				  << (m_name ? m_name : "?")
				  << "]::doCompare(" << file << ", " << cmp
				  << ") failed to seek to " << *ii *elem;
			UnixClose(fdes);
			hits.clear();
			return -3;
		    }
		    ierr = UnixRead(fdes, buf, elem*diff) / elem;
		    if (diff == ierr && ierr >= 0) {
			j = ierr;
		    }
		    else {
			j = 0;
			logWarning("doCompare", "expected to read %ld "
				   "integers from \"%s\" but got only %ld",
				   diff, file, ierr);
		    }
		    for (i = 0; i < j; ++i) {
			uint32_t k0 = ii[i] - *ii;
			if (cmp.inRange(buf[k0])) {
			    hits.setBit(ii[i], 1);
			}
		    }
		}
		else if (diff > 0) { // read one element at a time
		    for (i = 0; i < idx.nIndices(); ++i) {
			j = ii[i];
			diff = j * elem;
			ierr = UnixSeek(fdes, diff, SEEK_SET);
			if (ierr != diff) {
			    LOGGER(1) << "Warning -- ibis::part["
				      << (m_name ? m_name : "?")
				      << "]::doCompare(" << file << ", "
				      << cmp << ") failed to seek to " << diff;
			    UnixClose(fdes);
			    hits.clear();
			    return -4;
			}
			ierr = UnixRead(fdes, buf, elem);
			if (ierr > 0) {
			    if (cmp.inRange(*buf)) {
				hits.setBit(j, 1);
			    }
			}
			else {
			    LOGGER(1) << "Warning -- ibis::part["
				      << (m_name ? m_name : "?")
				      << "]::doCompare(" << file << ", "
				      << cmp << ") failed to read a value at "
				      << diff;
			}
		    }
		}
		ibis::fileManager::instance().recordPages
		    (elem*ii[0], elem*ii[idx.nIndices()-1]);
	    }
	    else { // read a single value
		j = *ii;
		diff = j * elem;
		ierr = UnixSeek(fdes, diff, SEEK_SET);
		if (ierr != diff) {
		    LOGGER(1) << "Warning -- ibis::part["
			      << (m_name ? m_name : "?")
			      << "]::doCompare(" << file << ", "
			      << cmp << ") failed to seek to " << diff;
		    UnixClose(fdes);
		    hits.clear();
		    return -4;
		}
		ierr = UnixRead(fdes, buf, elem);
		if (ierr > 0) {
		    ibis::fileManager::instance().recordPages(diff, diff+elem);
		    if (cmp.inRange(*buf)) {
			hits.setBit(j, 1);
		    }
		}
		else {
		    LOGGER(1) << "Warning -- ibis::part["
			      << (m_name ? m_name : "?")
			      << "]::doCompare(" << file << ", "
			      << cmp << ") failed to read a value at "
			      << diff;
		}
	    }

	    ++idx; // next set of selected entries
	} // while (idx.nIndices() > 0)
	delete [] buf;
    }
    else { // no user buffer to use, read a single value at a time
	T tmp;
	while (idx.nIndices() > 0) { // the outer loop
	    ii = idx.indices();
	    if (idx.isRange()) {
		diff = *ii * elem;
		ierr = UnixSeek(fdes, diff, SEEK_SET);
		if (ierr != diff) {
		    LOGGER(1) << "Warning -- ibis::part["
			      << (m_name ? m_name : "?")
			      << "]::doCompare(" << file << ", "
			      << cmp << ") failed to seek to " << diff;
		    UnixClose(fdes);
		    hits.clear();
		    return -5;
		}

		ibis::fileManager::instance().recordPages(diff, elem*ii[1]);
		j = ii[1];
		for (i = *ii; i < j; ++i) {
		    ierr = UnixRead(fdes, &tmp, elem);
		    if (ierr > 0 && cmp.inRange(tmp)) {
			hits.setBit(i, 1);
		    }
		}
	    }
	    else {
		for (i = 0; i < idx.nIndices(); ++i) {
		    j = ii[1];
		    diff = j * elem;
		    ierr = UnixSeek(fdes, diff, SEEK_SET);
		    if (ierr != diff) {
			LOGGER(1) << "Warning -- ibis::part["
				  << (m_name ? m_name : "?")
				  << "]::doCompare(" << file << ", "
				  << cmp << ") failed to seek to " << diff;
			UnixClose(fdes);
			hits.clear();
			return -6;
		    }

		    ierr = UnixRead(fdes, &tmp, elem);
		    if (ierr > 0 && cmp.inRange(tmp)) {
			hits.setBit(j, 1);
		    }
		}
		ibis::fileManager::instance().recordPages
		    (elem*ii[0], elem*ii[idx.nIndices()-1]);
	    }

	    ++idx; // next set of selected entries
	} // while (idx.nIndices() > 0)
    }

    UnixClose(fdes);
    if (uncomp)
	hits.compress();
    else if (hits.size() < nEvents)
	hits.adjustSize(0, nEvents);

    if (ibis::gVerbose > 1) {
	timer.stop();
	ibis::util::logger lg;
	lg.buffer() << "ibis::part[" << (m_name ? m_name : "?")
		    << "]::doCompare -- performing comparison "
		    << cmp << " on " << mask.cnt() << ' ' << typeid(T).name()
		    << "s from file \"" << file << "\" took "
		    << timer.realTime() << " sec elapsed time and produced "
		    << hits.cnt() << " hits" << "\n";
#if defined(DEBUG) && DEBUG + 0 > 1
	lg.buffer() << "mask\n" << mask << "\nhit vector\n" << hits << "\n";
#endif
    }
    return ierr;
} // ibis::part::doCompare

// hits are those do not satisfy the speficied range condition
template <typename T>
long ibis::part::negativeCompare(const array_t<T>& array,
				 const ibis::bitvector& mask,
				 ibis::bitvector& hits,
				 const ibis::qRange& cmp) const {
    ibis::horometer timer;
    if (ibis::gVerbose > 1) timer.start(); // start the timer

    uint32_t i=0, j=0;
    long ierr=0;
    const uint32_t nelm = (array.size() <= nEvents ? array.size() : nEvents);
    const bool uncomp = ((mask.size() >> 8) < mask.cnt());
    if (uncomp) { // use uncompressed hits internally
	hits.set(0, mask.size());
	hits.decompress();
    }
    else {
	hits.clear();
	hits.reserve(mask.size(), mask.cnt());
    }

    ibis::bitvector::indexSet idx = mask.firstIndexSet();
    while (idx.nIndices() > 0) { // the outer loop
	const ibis::bitvector::word_t *ii = idx.indices();
	if (idx.isRange()) {
	    uint32_t diff = (ii[1] <= nelm ? ii[1] : nelm);
	    for (j = *ii; j < diff; ++j) {
		if (! cmp.inRange(array[j])) {
		    hits.setBit(j, 1);
		    ++ ierr;
#if defined(DEBUG) && DEBUG + 0 > 1
		    LOGGER(0)
			<< "DEBUG: negativeCompare " << array[j]
			<< " is not in " << cmp;
#endif
		}
	    }
	}
	else {
	    for (i = 0; i < idx.nIndices(); ++i) {
		j = ii[i];
		if (j < nelm && ! cmp.inRange(array[j])) {
		    hits.setBit(j, 1);
		    ++ ierr;
#if defined(DEBUG) && DEBUG + 0 > 1
		    LOGGER(0)
			<< "DEBUG: negativeCompare " << array[j]
			<< " is no in " << cmp;
#endif
		}
	    }
	}

	++idx;
    }

    if (uncomp)
	hits.compress();
    else if (hits.size() < nEvents)
	hits.setBit(nEvents-1, 0);

    if (ibis::gVerbose > 1) {
	timer.stop();
	ibis::util::logger lg;
	lg.buffer() << "ibis::part[" << (m_name ? m_name : "?")
		    << "]::negativeCompare -- performing comparison "
		    << cmp << " on " << mask.cnt() << ' ' << typeid(T).name()
		    << "s from an array of "
		    << array.size() << " elements took "
		    << timer.realTime() << " sec elapsed time and produced "
		    << hits.cnt() << " hits" << "\n";
#if defined(DEBUG) && DEBUG + 0 > 1
	lg.buffer() << "mask\n" << mask << "\nhit vector\n" << hits << "\n";
#endif
    }
    return ierr;
} // ibis::part::negativeCompare

template <typename T>
long ibis::part::negativeCompare(const char* file,
				 const ibis::bitvector& mask,
				 ibis::bitvector& hits,
				 const ibis::qRange& cmp) const {
    ibis::horometer timer;
    if (ibis::gVerbose > 1)
	timer.start(); // start the timer

    hits.clear(); // clear the existing content
    int fdes = UnixOpen(file, OPEN_READONLY);
    if (fdes < 0) {
	logWarning("negativeCompare", "unable to open file \"%s\"",
		   file);
	hits.set(0, mask.size());
	return -1;
    }
#if defined(_WIN32) && defined(_MSC_VER)
    (void)_setmode(fdes, _O_BINARY);
#endif

    const unsigned elem = sizeof(T);
    // attempt to allocate a decent size buffer for operations
    char *cbuf;
    uint32_t nbuf = ibis::util::getBuffer(cbuf) / elem;
    T *buf = reinterpret_cast<T*>(cbuf);

    uint32_t i=0, j=0;
    long diff, ierr;
    const ibis::bitvector::word_t *ii;
    const bool uncomp = ((mask.size() >> 8) < mask.cnt());
    if (uncomp) { // use uncompressed hits internally
	hits.set(0, mask.size());
	hits.decompress();
    }
    else {
	hits.clear();
	hits.reserve(mask.size(), mask.cnt());
    }

    ibis::bitvector::indexSet idx = mask.firstIndexSet();

    if (buf) { // has a good size buffer to read data into
	while (idx.nIndices() > 0) { // the outer loop
	    ii = idx.indices();
	    if (idx.isRange()) {
		diff = *ii * elem;
		ierr = UnixSeek(fdes, diff, SEEK_SET);
		if (ierr != diff) {
		    LOGGER(1) << "Warning -- ibis::part["
			      << (m_name ? m_name : "?")
			      << "]::negativeCompare(" << file << ", " << cmp
			      << ") failed to seek to " << diff;
		    UnixClose(fdes);
		    hits.clear();
		    return -3;
		}

		ibis::fileManager::instance().recordPages
		    (diff, elem*ii[1]);
		j = ii[1];
		for (i = *ii; i < ii[1]; i += diff) {
		    diff = nbuf;
		    if (i+diff > ii[1])
			diff = ii[1] - i;
		    ierr = UnixRead(fdes, buf, elem*diff) / elem;
		    if (ierr > 0 && static_cast<uint32_t>(diff) == diff) {
			j = diff;
		    }
		    else {
			j = 0;
			logWarning("negativeCompare", "expected to read %ld "
				   "integers from \"%s\" but got only %ld",
				   diff, file, ierr);
		    }
		    for (uint32_t k = 0; k < j; ++k) {
			if (! cmp.inRange(buf[k])) {
			    hits.setBit(i+k, 1);
			}
		    }
		}
	    }
	    else if (idx.nIndices() > 1) {
		j = idx.nIndices() - 1;
		diff = ii[j] - *ii + 1;
		if (static_cast<uint32_t>(diff) < nbuf) {
		    ierr = UnixSeek(fdes, *ii * elem, SEEK_SET);
		    if (ierr != *ii * elem) {
			LOGGER(1) << "Warning -- ibis::part["
				  << (m_name ? m_name : "?")
				  << "]::negativeCompare(" << file << ", "
				  << cmp << ") failed to seek to " << *ii *elem;
			UnixClose(fdes);
			hits.clear();
			return -4;
		    }

		    ierr = UnixRead(fdes, buf, elem*diff) / elem;
		    if (ierr > 0 && ierr == diff) {
			j = diff;
		    }
		    else {
			j = 0;
			LOGGER(1) << "Warning -- ibis::part["
				  << (m_name ? m_name : "?")
				  << "]::negativeCompare(" << file << ", "
				  << cmp << ") expected to read " << diff
				  << " elements of " << elem << "-byte each, "
				  << "but got " << ierr;
		    }
		    for (i = 0; i < j; ++i) {
			uint32_t k0 = ii[i] - *ii;
			if (! cmp.inRange(buf[k0])) {
			    hits.setBit(ii[i], 1);
			}
		    }
		}
		else {
		    for (i = 0; i < idx.nIndices(); ++i) {
			j = ii[i];
			diff = j * elem;
			ierr = UnixSeek(fdes, diff, SEEK_SET);
			if (ierr != diff) {
			    LOGGER(1) << "Warning -- ibis::part["
				      << (m_name ? m_name : "?")
				      << "]::negativeCompare(" << file << ", "
				      << cmp << ") failed to seek to " << diff;
			    UnixClose(fdes);
			    hits.clear();
			    return -5;
			}
			ierr = UnixRead(fdes, buf, elem);
			if (ierr > 0) {
			    if (! cmp.inRange(*buf)) {
				hits.setBit(j, 1);
			    }
			}
		    }
		}
		ibis::fileManager::instance().recordPages
		    (elem*ii[0], elem*ii[idx.nIndices()-1]);
	    }
	    else {
		j = *ii;
		diff = j * elem;
		ierr = UnixSeek(fdes, diff, SEEK_SET);
		if (ierr != diff) {
		    LOGGER(1) << "Warning -- ibis::part["
			      << (m_name ? m_name : "?")
			      << "]::negativeCompare(" << file << ", "
			      << cmp << ") failed to seek to " << diff;
		    UnixClose(fdes);
		    hits.clear();
		    return -6;
		}
		ierr = UnixRead(fdes, buf, elem);
		if (ierr > 0) {
		    ibis::fileManager::instance().recordPages(diff, diff+elem);
		    if (! cmp.inRange(*buf)) {
			hits.setBit(j, 1);
		    }
		}
	    }

	    ++idx; // next set of selected entries
	} // while (idx.nIndices() > 0)
	delete [] buf;
    }
    else { // no user buffer to use, read one element at a time
	T tmp;
	while (idx.nIndices() > 0) { // the outer loop
	    ii = idx.indices();
	    if (idx.isRange()) {
		diff = *ii * elem;
		ierr = UnixSeek(fdes, diff, SEEK_SET);
		if (ierr != diff) {
		    LOGGER(1) << "Warning -- ibis::part["
			      << (m_name ? m_name : "?")
			      << "]::negativeCompare(" << file << ", "
			      << cmp << ") failed to seek to " << diff;
		    UnixClose(fdes);
		    hits.clear();
		    return -7;
		}
		ibis::fileManager::instance().recordPages(diff, elem*ii[1]);
		j = ii[1];
		for (i = *ii; i < j; ++i) {
		    ierr = UnixRead(fdes, &tmp, elem);
		    if (ierr > 0) {
			if (! cmp.inRange(tmp)) {
			    hits.setBit(i, 1);
			}
		    }
		}
	    }
	    else {
		for (i = 0; i < idx.nIndices(); ++i) {
		    j = ii[1];
		    diff = j * elem;
		    ierr = UnixSeek(fdes, diff, SEEK_SET);
		    if (ierr != diff) {
			LOGGER(1) << "Warning -- ibis::part["
				  << (m_name ? m_name : "?")
				  << "]::negativeCompare(" << file << ", "
				  << cmp << ") failed to seek to " << diff;
			UnixClose(fdes);
			hits.clear();
			return -8;
		    }
		    ierr = UnixRead(fdes, &tmp, elem);
		    if (ierr > 0) {
			if (! cmp.inRange(tmp)) {
			    hits.setBit(j, 1);
			}
		    }
		}
		ibis::fileManager::instance().recordPages
		    (elem*ii[0], elem*ii[idx.nIndices()-1]);
	    }

	    ++idx; // next set of selected entries
	} // while (idx.nIndices() > 0)
    }

    UnixClose(fdes);
    if (uncomp)
	hits.compress();
    else if (hits.size() < nEvents)
	hits.setBit(nEvents-1, 0);

    if (ibis::gVerbose > 1) {
	timer.stop();
	ibis::util::logger lg;
	lg.buffer() << "ibis::part[" << (m_name ? m_name : "?")
		    << "]::negativeCompare -- performing comparison "
		    << cmp << " on " << mask.cnt() << ' ' << typeid(T).name()
		    << "s from file \"" << file << "\" took "
		    << timer.realTime() << " sec elapsed time and produced "
		    << hits.cnt() << " hits" << "\n";
#if defined(DEBUG) && DEBUG + 0 > 1
	lg.buffer() << "mask\n" << mask << "\nhit vector\n" << hits << "\n";
#endif
    }
    ierr = hits.cnt();
    return ierr;
} // ibis::part::negativeCompare

// perform the actual comparison for range queries on file directly (without
// file mapping) assuming the file contains signed integers in binary
template <typename T>
long ibis::part::doScan(const array_t<T>& vals,
			const ibis::qContinuousRange& rng,
			const ibis::bitvector& mask,
			ibis::bitvector& hits) const {
    long ierr = -2;
    ibis::horometer timer;
    if (ibis::gVerbose > 1)
	timer.start();
    const bool uncomp = ((mask.size() >> 8) < mask.cnt());
    switch (rng.leftOperator()) {
    case ibis::qExpr::OP_LT: {
	switch (rng.rightOperator()) {
	case ibis::qExpr::OP_LT: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st<std::less<T> >
				  (std::less<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::less<T> >
				  (std::less<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st<std::less<T> >
				 (std::less<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::less<T> >
				 (std::less<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_LE: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st<std::less<T> >
				  (std::less<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::less_equal<T> >
				  (std::less_equal<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st<std::less<T> >
				 (std::less<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::less_equal<T> >
				 (std::less_equal<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_GT: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st<std::less<T> >
				  (std::less<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::greater<T> >
				  (std::greater<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st<std::less<T> >
				 (std::less<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::greater<T> >
				 (std::greater<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_GE: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st<std::less<T> >
				  (std::less<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::greater_equal<T> >
				  (std::greater_equal<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st<std::less<T> >
				 (std::less<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::greater_equal<T> >
				 (std::greater_equal<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_EQ: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st<std::less<T> >
				  (std::less<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::equal_to<T> >
				  (std::equal_to<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st<std::less<T> >
				 (std::less<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::equal_to<T> >
				 (std::equal_to<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	default: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st<std::less<T> >
				  (std::less<T>(),
				   static_cast<T>(rng.leftBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st<std::less<T> >
				 (std::less<T>(),
				  static_cast<T>(rng.leftBound())),
				 mask, hits);
	    break;}
	}
	break;}
    case ibis::qExpr::OP_LE: {
	switch (rng.rightOperator()) {
	case ibis::qExpr::OP_LT: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st<std::less_equal<T> >
				  (std::less_equal<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::less<T> >
				  (std::less<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st<std::less_equal<T> >
				 (std::less_equal<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::less<T> >
				 (std::less<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_LE: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st<std::less_equal<T> >
				  (std::less_equal<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::less_equal<T> >
				  (std::less_equal<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st<std::less_equal<T> >
				 (std::less_equal<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::less_equal<T> >
				 (std::less_equal<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_GT: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st<std::less_equal<T> >
				  (std::less_equal<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::greater<T> >
				  (std::greater<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st<std::less_equal<T> >
				 (std::less_equal<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::greater<T> >
				 (std::greater<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_GE: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st<std::less_equal<T> >
				  (std::less_equal<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::greater_equal<T> >
				  (std::greater_equal<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st<std::less_equal<T> >
				 (std::less_equal<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::greater_equal<T> >
				 (std::greater_equal<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_EQ: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st<std::less_equal<T> >
				  (std::less_equal<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::equal_to<T> >
				  (std::equal_to<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st<std::less_equal<T> >
				 (std::less_equal<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::equal_to<T> >
				 (std::equal_to<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	default: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st<std::less_equal<T> >
				  (std::less_equal<T>(),
				   static_cast<T>(rng.leftBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st<std::less_equal<T> >
				 (std::less_equal<T>(),
				  static_cast<T>(rng.leftBound())),
				 mask, hits);
	    break;}
	}
	break;}
    case ibis::qExpr::OP_GT: {
	switch (rng.rightOperator()) {
	case ibis::qExpr::OP_LT: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st< std::greater<T> >
				  (std::greater<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::less<T> >
				  (std::less<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st< std::greater<T> >
				 (std::greater<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::less<T> >
				 (std::less<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_LE: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st< std::greater<T> >
				  (std::greater<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::less_equal<T> >
				  (std::less_equal<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st< std::greater<T> >
				 (std::greater<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::less_equal<T> >
				 (std::less_equal<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_GT: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st< std::greater<T> >
				  (std::greater<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::greater<T> >
				  (std::greater<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st< std::greater<T> >
				 (std::greater<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::greater<T> >
				 (std::greater<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_GE: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st< std::greater<T> >
				  (std::greater<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::greater_equal<T> >
				  (std::greater_equal<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st< std::greater<T> >
				 (std::greater<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::greater_equal<T> >
				 (std::greater_equal<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_EQ: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st< std::greater<T> >
				  (std::greater<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::equal_to<T> >
				  (std::equal_to<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st< std::greater<T> >
				 (std::greater<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::equal_to<T> >
				 (std::equal_to<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	default: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st< std::greater<T> >
				  (std::greater<T>(),
				   static_cast<T>(rng.leftBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st< std::greater<T> >
				 (std::greater<T>(),
				  static_cast<T>(rng.leftBound())),
				 mask, hits);
	    break;}
	}
	break;}
    case ibis::qExpr::OP_GE: {
	switch (rng.rightOperator()) {
	case ibis::qExpr::OP_LT: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st< std::greater_equal<T> >
				  (std::greater_equal<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::less<T> >
				  (std::less<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st< std::greater_equal<T> >
				 (std::greater_equal<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::less<T> >
				 (std::less<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_LE: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st< std::greater_equal<T> >
				  (std::greater_equal<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::less_equal<T> >
				  (std::less_equal<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st< std::greater_equal<T> >
				 (std::greater_equal<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::less_equal<T> >
				 (std::less_equal<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_GT: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st< std::greater_equal<T> >
				  (std::greater_equal<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::greater<T> >
				  (std::greater<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st< std::greater_equal<T> >
				 (std::greater_equal<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::greater<T> >
				 (std::greater<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_GE: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st< std::greater_equal<T> >
				  (std::greater_equal<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::greater_equal<T> >
				  (std::greater_equal<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st< std::greater_equal<T> >
				 (std::greater_equal<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::greater_equal<T> >
				 (std::greater_equal<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_EQ: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st< std::greater_equal<T> >
				  (std::greater_equal<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::equal_to<T> >
				  (std::equal_to<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st< std::greater_equal<T> >
				 (std::greater_equal<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::equal_to<T> >
				 (std::equal_to<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	default: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st< std::greater_equal<T> >
				  (std::greater_equal<T>(),
				   static_cast<T>(rng.leftBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st< std::greater_equal<T> >
				 (std::greater_equal<T>(),
				  static_cast<T>(rng.leftBound())),
				 mask, hits);
	    break;}
	}
	break;}
    case ibis::qExpr::OP_EQ: {
	switch (rng.rightOperator()) {
	case ibis::qExpr::OP_LT: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st< std::equal_to<T> >
				  (std::equal_to<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::less<T> >
				  (std::less<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st< std::equal_to<T> >
				 (std::equal_to<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::less<T> >
				 (std::less<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_LE: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st< std::equal_to<T> >
				  (std::equal_to<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::less_equal<T> >
				  (std::less_equal<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st< std::equal_to<T> >
				 (std::equal_to<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::less_equal<T> >
				 (std::less_equal<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_GT: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st< std::equal_to<T> >
				  (std::equal_to<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::greater<T> >
				  (std::greater<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st< std::equal_to<T> >
				 (std::equal_to<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::greater<T> >
				 (std::greater<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_GE: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st< std::equal_to<T> >
				  (std::equal_to<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::greater_equal<T> >
				  (std::greater_equal<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st< std::equal_to<T> >
				 (std::equal_to<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::greater_equal<T> >
				 (std::greater_equal<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_EQ: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st< std::equal_to<T> >
				  (std::equal_to<T>(),
				   static_cast<T>(rng.leftBound())),
				  std::binder2nd<std::equal_to<T> >
				  (std::equal_to<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st< std::equal_to<T> >
				 (std::equal_to<T>(),
				  static_cast<T>(rng.leftBound())),
				 std::binder2nd<std::equal_to<T> >
				 (std::equal_to<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	default: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder1st< std::equal_to<T> >
				  (std::equal_to<T>(),
				   static_cast<T>(rng.leftBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder1st< std::equal_to<T> >
				 (std::equal_to<T>(),
				  static_cast<T>(rng.leftBound())),
				 mask, hits);
	    break;}
	}
	break;}
    default: {
	switch (rng.rightOperator()) {
	case ibis::qExpr::OP_LT: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder2nd<std::less<T> >
				  (std::less<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder2nd<std::less<T> >
				 (std::less<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_LE: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder2nd<std::less_equal<T> >
				  (std::less_equal<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder2nd<std::less_equal<T> >
				 (std::less_equal<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_GT: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder2nd<std::greater<T> >
				  (std::greater<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder2nd<std::greater<T> >
				 (std::greater<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_GE: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder2nd<std::greater_equal<T> >
				  (std::greater_equal<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder2nd<std::greater_equal<T> >
				 (std::greater_equal<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	case ibis::qExpr::OP_EQ: {
	    if (uncomp)
		ierr = doCompare0(vals,
				  std::binder2nd<std::equal_to<T> >
				  (std::equal_to<T>(),
				   static_cast<T>(rng.rightBound())),
				  mask, hits);
	    else
		ierr = doCompare(vals,
				 std::binder2nd<std::equal_to<T> >
				 (std::equal_to<T>(),
				  static_cast<T>(rng.rightBound())),
				 mask, hits);
	    break;}
	default: {
	    ierr = mask.cnt();
	    hits.copy(mask);
	    break;}
	}
	break;}
    }

    if (ibis::gVerbose > 1) {
	timer.stop();
	ibis::util::logger lg;
	lg.buffer() << "ibis::part[" << (m_name ? m_name : "?")
		    << "]::doScan -- evaluating "
		    << rng << " on " << mask.cnt() << " " << typeid(T).name()
		    << (mask.cnt() > 1 ? " values" : " value") << " (total: "
		    << nEvents << ") took " << timer.realTime()
		    << " sec elapsed time and produced " << hits.cnt()
		    << (hits.cnt() > 1 ? " hits" : " hit") << "\n";
#if defined(DEBUG) && DEBUG + 0 > 1
	lg.buffer() << "mask\n" << mask << "\n";
	lg.buffer() << "hit vector\n" << hits << "\n";
#endif
    }
    return ierr;
} // ibis::part::doScan

/// Accepts an externally passed comparison operator. It chooses whether
/// the bitvector @c hits will be compressed internally based on the number
/// of set bits in the @c mask.
template <typename T, typename F>
long ibis::part::doCompare(const array_t<T>& vals, F cmp,
			   const ibis::bitvector& mask,
			   ibis::bitvector& hits) const {
    long ierr = 0;
    if (mask.size() == 0 || mask.cnt() == 0)
	return ierr;

    if (vals.size() != mask.size() && vals.size() != mask.cnt()) {
	if (ibis::gVerbose > 0)
	    logWarning("doCompare", "array_t<%s>.size(%lu) must be either "
		       "%lu or %lu", typeid(T).name(),
		       static_cast<long unsigned>(vals.size()),
		       static_cast<long unsigned>(mask.size()),
		       static_cast<long unsigned>(mask.cnt()));
	ierr = -1;
	return ierr;
    }

    const bool uncomp = ((mask.size() >> 8) < mask.cnt());
    if (uncomp) { // use uncompressed hits internally
	hits.set(0, mask.size());
	hits.decompress();
    }
    else {
	hits.clear();
	hits.reserve(mask.size(), mask.cnt());
    }
    if (vals.size() == mask.size()) { // full list of values
	for (ibis::bitvector::indexSet ix = mask.firstIndexSet();
	     ix.nIndices() > 0; ++ ix) {
	    const ibis::bitvector::word_t *iix = ix.indices();
	    if (ix.isRange()) {
		for (unsigned j = *iix; j < iix[1]; ++ j) {
		    if (cmp(vals[j]))
			hits.setBit(j, 1);
		}
	    }
	    else {
		for (unsigned j = 0; j < ix.nIndices(); ++ j) {
		    if (cmp(vals[iix[j]]))
			hits.setBit(iix[j], 1);
		}
	    }
	}
    }
    else { // compacted values
	unsigned ival = 0;
	for (ibis::bitvector::indexSet ix = mask.firstIndexSet();
	     ix.nIndices() > 0; ++ ix) {
	    const ibis::bitvector::word_t *iix = ix.indices();
	    if (ix.isRange()) {
		for (unsigned j = *iix; j < iix[1]; ++ j) {
		    if (cmp(vals[ival]))
			hits.setBit(j, 1);
		    ++ ival;
		}
	    }
	    else {
		for (unsigned j = 0; j < ix.nIndices(); ++ j) {
		    if (cmp(vals[ival]))
			hits.setBit(iix[j], 1);
		    ++ ival;
		}
	    }
	}
    }

    if (uncomp)
	hits.compress();
    else
	hits.adjustSize(0, mask.size());
    ierr = hits.cnt();
    return ierr;
} // ibis::part::doCompare

/// This version uses an uncompressed bitvector to store the scan results
/// internally.
template <typename T, typename F>
long ibis::part::doCompare0(const array_t<T>& vals, F cmp,
			    const ibis::bitvector& mask,
			    ibis::bitvector& hits) const {
    long ierr = 0;
    if (mask.size() == 0 || mask.cnt() == 0)
	return ierr;

    if (vals.size() != mask.size() && vals.size() != mask.cnt()) {
	if (ibis::gVerbose > 0)
	    logWarning("doCompare", "array_t<%s>.size(%lu) must be either "
		       "%lu or %lu", typeid(T).name(),
		       static_cast<long unsigned>(vals.size()),
		       static_cast<long unsigned>(mask.size()),
		       static_cast<long unsigned>(mask.cnt()));
	ierr = -1;
	return ierr;
    }

    hits.set(0, mask.size());
    hits.decompress();
    if (vals.size() == mask.size()) { // full list of values
	for (ibis::bitvector::indexSet ix = mask.firstIndexSet();
	     ix.nIndices() > 0; ++ ix) {
	    const ibis::bitvector::word_t *iix = ix.indices();
	    if (ix.isRange()) {
		for (unsigned j = *iix; j < iix[1]; ++ j) {
		    if (cmp(vals[j]))
			hits.turnOnRawBit(j);
		}
	    }
	    else {
		for (unsigned j = 0; j < ix.nIndices(); ++ j) {
		    if (cmp(vals[iix[j]]))
			hits.turnOnRawBit(iix[j]);
		}
	    }
	}
    }
    else { // compacted values
	unsigned ival = 0;
	for (ibis::bitvector::indexSet ix = mask.firstIndexSet();
	     ix.nIndices() > 0; ++ ix) {
	    const ibis::bitvector::word_t *iix = ix.indices();
	    if (ix.isRange()) {
		for (unsigned j = *iix; j < iix[1]; ++ j) {
		    if (cmp(vals[ival]))
			hits.turnOnRawBit(j);
		    ++ ival;
		}
	    }
	    else {
		for (unsigned j = 0; j < ix.nIndices(); ++ j) {
		    if (cmp(vals[ival]))
			hits.turnOnRawBit(iix[j]);
		    ++ ival;
		}
	    }
	}
    }

    hits.compress();
    ierr = hits.cnt();
    return ierr;
} // ibis::part::doCompare0

/// The actual scan function.  This one chooses whether the internal
/// bitvector for storing the scan results will be compressed or not.  It
/// always returns a compressed bitvector.
template <typename T, typename F1, typename F2>
long ibis::part::doCompare(const array_t<T>& vals, F1 cmp1, F2 cmp2,
			   const ibis::bitvector& mask,
			   ibis::bitvector& hits) const {
    long ierr = 0;
    if (mask.size() == 0 || mask.cnt() == 0)
	return ierr;

    if (vals.size() != mask.size() && vals.size() != mask.cnt()) {
	if (ibis::gVerbose > 0)
	    logWarning("doCompare", "array_t<%s>.size(%lu) must be either "
		       "%lu or %lu", typeid(T).name(),
		       static_cast<long unsigned>(vals.size()),
		       static_cast<long unsigned>(mask.size()),
		       static_cast<long unsigned>(mask.cnt()));
	ierr = -1;
	return ierr;
    }

    const bool uncomp = ((mask.size() >> 8) < mask.cnt());
    if (uncomp) { // use uncompressed hits internally
	hits.set(0, mask.size());
	hits.decompress();
    }
    else {
	hits.clear();
	hits.reserve(mask.size(), mask.cnt());
    }
    if (vals.size() == mask.size()) { // full list of values
	for (ibis::bitvector::indexSet ix = mask.firstIndexSet();
	     ix.nIndices() > 0; ++ ix) {
	    const ibis::bitvector::word_t *iix = ix.indices();
	    if (ix.isRange()) {
		for (unsigned j = *iix; j < iix[1]; ++ j) {
		    if (cmp1(vals[j]) && cmp2(vals[j]))
			hits.setBit(j, 1);
		}
	    }
	    else {
		for (unsigned j = 0; j < ix.nIndices(); ++ j) {
		    if (cmp1(vals[iix[j]]) && cmp2(vals[iix[j]]))
			hits.setBit(iix[j], 1);
		}
	    }
	}
    }
    else { // compacted values
	unsigned ival = 0;
	for (ibis::bitvector::indexSet ix = mask.firstIndexSet();
	     ix.nIndices() > 0; ++ ix) {
	    const ibis::bitvector::word_t *iix = ix.indices();
	    if (ix.isRange()) {
		for (unsigned j = *iix; j < iix[1]; ++ j) {
		    if (cmp1(vals[ival]) && cmp2(vals[ival]))
			hits.setBit(j, 1);
		    ++ ival;
		}
	    }
	    else {
		for (unsigned j = 0; j < ix.nIndices(); ++ j) {
		    if (cmp1(vals[ival]) && cmp2(vals[ival]))
			hits.setBit(iix[j], 1);
		    ++ ival;
		}
	    }
	}
    }

    if (uncomp)
	hits.compress();
    else
	hits.adjustSize(0, mask.size());
    ierr = hits.cnt();
    return ierr;
} // ibis::part::doCompare

/// This version uses uncompressed bitvector to store the scan results
/// internally.
template <typename T, typename F1, typename F2>
long ibis::part::doCompare0(const array_t<T>& vals, F1 cmp1, F2 cmp2,
			    const ibis::bitvector& mask,
			    ibis::bitvector& hits) const {
    long ierr = 0;
    if (mask.size() == 0 || mask.cnt() == 0)
	return ierr;

    if (vals.size() != mask.size() && vals.size() != mask.cnt()) {
	if (ibis::gVerbose > 0)
	    logWarning("doCompare", "array_t<%s>.size(%lu) must be either "
		       "%lu or %lu", typeid(T).name(),
		       static_cast<long unsigned>(vals.size()),
		       static_cast<long unsigned>(mask.size()),
		       static_cast<long unsigned>(mask.cnt()));
	ierr = -1;
	return ierr;
    }

    hits.set(0, mask.size());
    hits.decompress();
    if (vals.size() == mask.size()) { // full list of values
	for (ibis::bitvector::indexSet ix = mask.firstIndexSet();
	     ix.nIndices() > 0; ++ ix) {
	    const ibis::bitvector::word_t *iix = ix.indices();
	    if (ix.isRange()) {
		for (unsigned j = *iix; j < iix[1]; ++ j) {
		    if (cmp1(vals[j]) && cmp2(vals[j]))
			hits.turnOnRawBit(j);
		}
	    }
	    else {
		for (unsigned j = 0; j < ix.nIndices(); ++ j) {
		    if (cmp1(vals[iix[j]]) && cmp2(vals[iix[j]]))
			hits.turnOnRawBit(iix[j]);
		}
	    }
	}
    }
    else { // compacted values
	unsigned ival = 0;
	for (ibis::bitvector::indexSet ix = mask.firstIndexSet();
	     ix.nIndices() > 0; ++ ix) {
	    const ibis::bitvector::word_t *iix = ix.indices();
	    if (ix.isRange()) {
		for (unsigned j = *iix; j < iix[1]; ++ j) {
		    if (cmp1(vals[ival]) && cmp2(vals[ival]))
			hits.turnOnRawBit(j);
		    ++ ival;
		}
	    }
	    else {
		for (unsigned j = 0; j < ix.nIndices(); ++ j) {
		    if (cmp1(vals[ival]) && cmp2(vals[ival]))
			hits.turnOnRawBit(iix[j]);
		    ++ ival;
		}
	    }
	}
    }

    hits.compress();
    ierr = hits.cnt();
    return ierr;
} // ibis::part::doCompare

long ibis::part::countHits(const ibis::qRange& cmp) const {
    columnList::const_iterator it = columns.find(cmp.colName());
    if (it == columns.end()) {
	logWarning("countHits", "unknown column name %s in query expression",
		   cmp.colName());
	return -1;
    }

    long ierr = 0;
    ibis::horometer timer;
    if (ibis::gVerbose > 1)
	timer.start();
    switch ((*it).second->type()) {
    case ibis::UBYTE:
	ierr = doCount<unsigned char>(cmp);
	break;
    case ibis::BYTE:
	ierr = doCount<signed char>(cmp);
	break;
    case ibis::USHORT:
	ierr = doCount<uint16_t>(cmp);
	break;
    case ibis::SHORT:
	ierr = doCount<int16_t>(cmp);
	break;
    case ibis::UINT:
	ierr = doCount<uint32_t>(cmp);
	break;
    case ibis::INT:
	ierr = doCount<int32_t>(cmp);
	break;
    case ibis::ULONG:
	ierr = doCount<uint64_t>(cmp);
	break;
    case ibis::LONG:
	ierr = doCount<int64_t>(cmp);
	break;
    case ibis::FLOAT:
	ierr = doCount<float>(cmp);
	break;
    case ibis::DOUBLE:
	ierr = doCount<double>(cmp);
	break;
    default:
	if (ibis::gVerbose> -1)
	    logWarning("countHits", "does not support type %d (%s)",
		       static_cast<int>((*it).second->type()),
		       cmp.colName());
	ierr = -4;
	break;
    }

    if (ibis::gVerbose > 1) {
	timer.stop();
	ibis::util::logger lg;
	lg.buffer() << "ibis::part[" << (m_name ? m_name : "?")
		    << "]::countHits -- evaluating "
		    << cmp << " on " << nEvents << " records took "
		    << timer.realTime()
		    << " sec elapsed time and produced "
		    << ierr << (ierr > 1 ? " hits" : "hit") << "\n";
    }
    return ierr;
} // ibis::part::countHits

template <typename T>
long ibis::part::doCount(const ibis::qRange& cmp) const {
    columnList::const_iterator it = columns.find(cmp.colName());
    if (it == columns.end()) {
	return -1;
    }

    char* filename = (*it).second->dataFileName();
    if (filename == 0)
	return -2;

    array_t<T> vals;
    long ierr = ibis::fileManager::instance().getFile(filename, vals);
    delete [] filename;
    if (ierr != 0) {
	return -3;
    }
    ibis::bitvector mask;
    (*it).second->getNullMask(mask);
    mask.adjustSize(0, vals.size());

    if (cmp.getType() == ibis::qExpr::RANGE) { // simple range
	const ibis::qContinuousRange& rng =
	    static_cast<const ibis::qContinuousRange&>(cmp);
	switch (rng.leftOperator()) {
	case ibis::qExpr::OP_LT: {
	    switch (rng.rightOperator()) {
	    case ibis::qExpr::OP_LT: {
		ierr = doCount(vals, mask,
			       std::binder1st<std::less<T> >
			       (std::less<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::less<T> >
			       (std::less<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_LE: {
		ierr = doCount(vals, mask,
			       std::binder1st<std::less<T> >
			       (std::less<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::less_equal<T> >
			       (std::less_equal<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_GT: {
		ierr = doCount(vals, mask,
			       std::binder1st<std::less<T> >
			       (std::less<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::greater<T> >
			       (std::greater<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_GE: {
		ierr = doCount(vals, mask,
			       std::binder1st<std::less<T> >
			       (std::less<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::greater_equal<T> >
			       (std::greater_equal<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_EQ: {
		ierr = doCount(vals, mask,
			       std::binder1st<std::less<T> >
			       (std::less<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::equal_to<T> >
			       (std::equal_to<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    default: {
		ierr = doCount(vals, mask,
			       std::binder1st<std::less<T> >
			       (std::less<T>(),
				static_cast<T>(rng.leftBound())));
		break;}
	    }
	    break;}
	case ibis::qExpr::OP_LE: {
	    switch (rng.rightOperator()) {
	    case ibis::qExpr::OP_LT: {
		ierr = doCount(vals, mask,
			       std::binder1st<std::less_equal<T> >
			       (std::less_equal<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::less<T> >
			       (std::less<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_LE: {
		ierr = doCount(vals, mask,
			       std::binder1st<std::less_equal<T> >
			       (std::less_equal<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::less_equal<T> >
			       (std::less_equal<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_GT: {
		ierr = doCount(vals, mask,
			       std::binder1st<std::less_equal<T> >
			       (std::less_equal<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::greater<T> >
			       (std::greater<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_GE: {
		ierr = doCount(vals, mask,
			       std::binder1st<std::less_equal<T> >
			       (std::less_equal<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::greater_equal<T> >
			       (std::greater_equal<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_EQ: {
		ierr = doCount(vals, mask,
			       std::binder1st<std::less_equal<T> >
			       (std::less_equal<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::equal_to<T> >
			       (std::equal_to<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    default: {
		ierr = doCount(vals, mask,
			       std::binder1st<std::less_equal<T> >
			       (std::less_equal<T>(),
				static_cast<T>(rng.leftBound())));
		break;}
	    }
	    break;}
	case ibis::qExpr::OP_GT: {
	    switch (rng.rightOperator()) {
	    case ibis::qExpr::OP_LT: {
		ierr = doCount(vals, mask,
			       std::binder1st< std::greater<T> >
			       (std::greater<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::less<T> >
			       (std::less<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_LE: {
		ierr = doCount(vals, mask,
			       std::binder1st< std::greater<T> >
			       (std::greater<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::less_equal<T> >
			       (std::less_equal<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_GT: {
		ierr = doCount(vals, mask,
			       std::binder1st< std::greater<T> >
			       (std::greater<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::greater<T> >
			       (std::greater<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_GE: {
		ierr = doCount(vals, mask,
			       std::binder1st< std::greater<T> >
			       (std::greater<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::greater_equal<T> >
			       (std::greater_equal<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_EQ: {
		ierr = doCount(vals, mask,
			       std::binder1st< std::greater<T> >
			       (std::greater<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::equal_to<T> >
			       (std::equal_to<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    default: {
		ierr = doCount(vals, mask,
			       std::binder1st< std::greater<T> >
			       (std::greater<T>(),
				static_cast<T>(rng.leftBound())));
		break;}
	    }
	    break;}
	case ibis::qExpr::OP_GE: {
	    switch (rng.rightOperator()) {
	    case ibis::qExpr::OP_LT: {
		ierr = doCount(vals, mask,
			       std::binder1st< std::greater_equal<T> >
			       (std::greater_equal<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::less<T> >
			       (std::less<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_LE: {
		ierr = doCount(vals, mask,
			       std::binder1st< std::greater_equal<T> >
			       (std::greater_equal<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::less_equal<T> >
			       (std::less_equal<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_GT: {
		ierr = doCount(vals, mask,
			       std::binder1st< std::greater_equal<T> >
			       (std::greater_equal<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::greater<T> >
			       (std::greater<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_GE: {
		ierr = doCount(vals, mask,
			       std::binder1st< std::greater_equal<T> >
			       (std::greater_equal<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::greater_equal<T> >
			       (std::greater_equal<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_EQ: {
		ierr = doCount(vals, mask,
			       std::binder1st< std::greater_equal<T> >
			       (std::greater_equal<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::equal_to<T> >
			       (std::equal_to<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    default: {
		ierr = doCount(vals, mask,
			       std::binder1st< std::greater_equal<T> >
			       (std::greater_equal<T>(),
				static_cast<T>(rng.leftBound())));
		break;}
	    }
	    break;}
	case ibis::qExpr::OP_EQ: {
	    switch (rng.rightOperator()) {
	    case ibis::qExpr::OP_LT: {
		ierr = doCount(vals, mask,
			       std::binder1st< std::equal_to<T> >
			       (std::equal_to<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::less<T> >
			       (std::less<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_LE: {
		ierr = doCount(vals, mask,
			       std::binder1st< std::equal_to<T> >
			       (std::equal_to<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::less_equal<T> >
			       (std::less_equal<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_GT: {
		ierr = doCount(vals, mask,
			       std::binder1st< std::equal_to<T> >
			       (std::equal_to<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::greater<T> >
			       (std::greater<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_GE: {
		ierr = doCount(vals, mask,
			       std::binder1st< std::equal_to<T> >
			       (std::equal_to<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::greater_equal<T> >
			       (std::greater_equal<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_EQ: {
		ierr = doCount(vals, mask,
			       std::binder1st< std::equal_to<T> >
			       (std::equal_to<T>(),
				static_cast<T>(rng.leftBound())),
			       std::binder2nd<std::equal_to<T> >
			       (std::equal_to<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    default: {
		ierr = doCount(vals, mask,
			       std::binder1st< std::equal_to<T> >
			       (std::equal_to<T>(),
				static_cast<T>(rng.leftBound())));
		break;}
	    }
	    break;}
	default: {
	    switch (rng.rightOperator()) {
	    case ibis::qExpr::OP_LT: {
		ierr = doCount(vals, mask,
			       std::binder2nd<std::less<T> >
			       (std::less<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_LE: {
		ierr = doCount(vals, mask,
			       std::binder2nd<std::less_equal<T> >
			       (std::less_equal<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_GT: {
		ierr = doCount(vals, mask,
			       std::binder2nd<std::greater<T> >
			       (std::greater<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_GE: {
		ierr = doCount(vals, mask,
			       std::binder2nd<std::greater_equal<T> >
			       (std::greater_equal<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    case ibis::qExpr::OP_EQ: {
		ierr = doCount(vals, mask,
			       std::binder2nd<std::equal_to<T> >
			       (std::equal_to<T>(),
				static_cast<T>(rng.rightBound())));
		break;}
	    default: {
		ierr = mask.cnt();
		break;}
	    }
	    break;}
	}
    }
    else {
	ierr = doCount(vals, cmp, mask);
    }
    return ierr;
} // ibis::part::doCount

template <typename T>
long ibis::part::doCount(const array_t<T>& vals, const ibis::qRange& cmp,
			 const ibis::bitvector& mask) const {
    long ierr = 0;
    for (ibis::bitvector::indexSet ix = mask.firstIndexSet();
	 ix.nIndices() > 0; ++ ix) {
	const ibis::bitvector::word_t *iix = ix.indices();
	if (ix.isRange()) {
	    for (unsigned ii = *iix; ii < iix[1]; ++ ii)
		ierr += static_cast<int>(cmp.inRange(vals[ii]));
	}
	else {
	    for (unsigned ii = 0; ii < ix.nIndices(); ++ ii)
		ierr += static_cast<int>(cmp.inRange(vals[iix[ii]]));
	}
    }
    return ierr;
} // ibis::part::doCount

template <typename T, typename F>
long ibis::part::doCount(const array_t<T>& vals,
			 const ibis::bitvector& mask, F cmp) const {
    long ierr = 0;
    for (ibis::bitvector::indexSet ix = mask.firstIndexSet();
	 ix.nIndices() > 0; ++ ix) {
	const ibis::bitvector::word_t *iix = ix.indices();
	if (ix.isRange()) {
	    for (unsigned ii = *iix; ii < iix[1]; ++ ii)
		ierr += static_cast<int>(cmp(vals[ii]));
	}
	else {
	    for (unsigned ii = 0; ii < ix.nIndices(); ++ ii)
		ierr += static_cast<int>(cmp(vals[iix[ii]]));
	}
    }
    return ierr;
} // ibis::part::doCount

template <typename T, typename F1, typename F2>
long ibis::part::doCount(const array_t<T>& vals,
			 const ibis::bitvector& mask,
			 F1 cmp1, F2 cmp2) const {
    long ierr = 0;
    for (ibis::bitvector::indexSet ix = mask.firstIndexSet();
	 ix.nIndices() > 0; ++ ix) {
	const ibis::bitvector::word_t *iix = ix.indices();
	if (ix.isRange()) {
	    for (unsigned ii = *iix; ii < iix[1]; ++ ii)
		ierr += static_cast<int>(cmp1(vals[ii]) && cmp2(vals[ii]));
	}
	else {
	    for (unsigned ii = 0; ii < ix.nIndices(); ++ ii)
		ierr += static_cast<int>(cmp1(vals[iix[ii]]) &&
					 cmp2(vals[iix[ii]]));
	}
    }
    return ierr;
} // ibis::part::doCount

// derive backupDir from activeDir
void ibis::part::deriveBackupDirName() {
    if (activeDir == 0) {
#if DIRSEP == '/'
	activeDir = ibis::util::strnewdup(".ibis/dir1");
	backupDir = ibis::util::strnewdup(".ibis/dir2");
#else
	activeDir = ibis::util::strnewdup(".ibis\\dir1");
	backupDir = ibis::util::strnewdup(".ibis\\dir2");
#endif
    }
    if (backupDir) {
	if (strcmp(activeDir, backupDir)) {
	    // activeDir differs from backupDir, no need to do anything
	    return;
	}
	delete [] backupDir;
	backupDir = 0;
    }

    uint32_t j = strlen(activeDir);
    backupDir = new char[j+12];
    (void) strcpy(backupDir, activeDir);
    char* ptr = backupDir + j - 1;
    while (ptr>=backupDir && isdigit(*ptr)) {
	-- ptr;
    }
    ++ ptr;
    if (ptr >= backupDir + j) {
	ptr = backupDir + j;
	j = 0;
    }
    else {
	j = atoi(ptr);
    }

    long ierr;
    do {
	++j;
	Stat_T buf;
	(void) sprintf(ptr, "%lu", static_cast<long unsigned>(j));
	ierr = UnixStat(backupDir, &buf);
    } while ((ierr == 0 || errno != ENOENT) && j);
    if (j == 0) {
	logError("deriveBackupDirName", "all names of the form %snnn are "
		 "in use", activeDir);
    }
} // ibis::part::deriveBackupDirName

// make sure the Alternative_Directory specified in the metadata files from
// the activeDir and the backupDir are consistent, and the number of events
// are the same
long ibis::part::verifyBackupDir() {
    long ierr = 0;
    if (activeDir == 0) return ierr;

    if (backupDir == 0 || *backupDir == 0 || backupDir == activeDir ||
	strcmp(activeDir, backupDir) == 0)
	return ierr;
    try {
	ierr = ibis::util::makeDir(backupDir);
	if (ierr < 0) return ierr;
    }
    catch (const std::exception& e) {
	logError("ibis::part::verifyBackupDir",
		 "unable to create backupDir \"%s\" -- %s",
		 backupDir, e.what());
    }
    catch (const char* s) {
	logError("ibis::part::verifyBackupDir",
		 "unable to create backupDir \"%s\" -- %s",
		 backupDir, s);
    }
    catch (...) {
	logError("ibis::part::verifyBackupDir",
		 "unable to create backupDir \"%s\" -- unknow error",
		 backupDir);
    }

    Stat_T st;
    uint32_t np = 0;
    std::string fn = backupDir;
    fn += DIRSEP;
    fn += "-part.txt";
    ierr = UnixStat(fn.c_str(), &st);
    if (ierr != 0) { // try the old file name
	fn.erase(fn.size()-10);
	fn += "table.tdc";
	ierr = UnixStat(fn.c_str(), &st);
    }
    if (ierr == 0) {
	// read the file to retrieve Alternative_Directory and
	// Number_of_events
	FILE* file = fopen(fn.c_str(), "r");
	if (file == 0) {
	    logWarning("verifyBackupDir", "unable to open file \"%s\" ... %s",
		       fn.c_str(), (errno ? strerror(errno)
				    : "no free stdio stream"));
	    if (nEvents == 0) ierr = 0;
	    return ierr;
	}

	char buf[MAX_LINE];
	char *rs;
	while (0 != (rs = fgets(buf, MAX_LINE, file))) {
	    rs = strchr(buf, '=');
	    if (strnicmp(buf, "END HEADER", 10) == 0) {
		break;
	    }
	    else if (rs != 0) {
		++ rs; // pass = character
		if (strnicmp(buf, "Number_of_rows", 14) == 0 ||
		    strnicmp(buf, "Number_of_events", 16) == 0 ||
		    strnicmp(buf, "Number_of_records", 17) == 0) {
		    uint32_t ne = atol(rs);
		    if (ne != nEvents) {
			-- ierr;
			logWarning("verifyBackupDir", "backup directory"
				   " contains %lu rows, but the active"
				   " directory has %lu.",
				   static_cast<long unsigned>(ne),
				   static_cast<long unsigned>(nEvents));
		    }
		}
		else if (strnicmp(buf, "Number_of_columns", 17) == 0 ||
			 strnicmp(buf, "Number_of_properties", 20) == 0) {
		    np = atol(rs);
		}
		else if (strnicmp(buf, "Alternative_Directory", 21) == 0) {
		    rs += strspn(rs," \t\"\'");
		    char* tmp = strpbrk(rs, " \t\"\'");
		    if (tmp != 0) *tmp = static_cast<char>(0);
		    if ((backupDir == 0 || strcmp(rs, backupDir)) &&
			(activeDir == 0 || strcmp(rs, activeDir))) {
			-- ierr;
			logWarning("verifyBackupDir",
				   "Alternative_Directory "
				   "entry inconsistent: active=\"%s\" "
				   "backup=\"%s\"", backupDir, rs);
		    }
		}
	    }
	} // while ...
	fclose(file);
    } // if (ierr == 0)
    else if (nEvents > 0) {
	logWarning("verifyBackupDir", "no metadata file in \"%s\".  The "
		   "backup directory is likely empty.\nstat returns %d, "
		   "errno = %d (%s).", backupDir, ierr, errno,
		   strerror(errno));
	ierr = -10;
    }
    else {
	ierr = 0;
    }
    if (ierr < 0)
	return ierr;
    ierr = 0;
    if (np != columns.size()) {
	ierr = -11;
	logWarning("verifyBackupDir", "backup directory "
		   "contains %lu columns, but the active "
		   "directory has %lu.", static_cast<long unsigned>(np),
		   static_cast<long unsigned>(columns.size()));
	return ierr;
    }

    if (ierr == 0) {
	if (ibis::gVerbose > 1)
	    logMessage("verifyBackupDir", "backupDir verified to be ok");
    }
    else {
	if (ibis::gVerbose > 0)
	    logWarning("verifyBackupDir", "backupDir verified to be NOT ok. "
		       "ierr = %d", ierr);
	ierr -= 100;
    }
    return ierr;
} // ibis::part::verifyBackupDir

/// The routine to perform the actual copying for making a backup copy.
void ibis::part::doBackup() {
    if (backupDir == 0 || *backupDir == 0 || activeDir == 0)
	return;

    if ((state == UNKNOWN_STATE || state == PRETRANSITION_STATE ||
	 state == POSTTRANSITION_STATE) && nEvents > 0) {
	// only make copy nonempty tables in some states
#if (defined(unix) && !defined(__CYGWIN__) && !(defined(sun) && defined(__GNUC__) && __GNUC__ <= 2)) || defined(__HOS_AIX__)
	// block SIGHUP and SIGINT to this thread
	sigset_t sigs;
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGHUP);
	sigaddset(&sigs, SIGINT);
	pthread_sigmask(SIG_BLOCK, &sigs, 0);
#endif
	{
	    ibis::util::mutexLock lck(&ibis::util::envLock, backupDir);
	    ibis::util::removeDir(backupDir);
	}
	if (ibis::gVerbose > 2)
	    logMessage("doBackup", "copy files from \"%s\" to \"%s\"",
		       activeDir, backupDir);

	char* cmd = new char[strlen(activeDir)+strlen(backupDir)+32];
#if defined(unix) || defined(linux) || defined(__HOS_AIX__) || defined(__APPLE__) || defined(__FreeBSD__)
	sprintf(cmd, "/bin/cp -fr \"%s\" \"%s\"", activeDir, backupDir);
#elif defined(_WIN32)
	sprintf(cmd, "xcopy /i /s /e /h /r /q \"%s\" \"%s\"", activeDir,
		backupDir);
#else
#error DO NOT KNOW HOW TO COPY FILES!
#endif
	if (ibis::gVerbose > 4)
	    logMessage("doBackup", "issuing sh command \"%s\"..", cmd);

	FILE* fptr = popen(cmd, "r");
	if (fptr) {
	    char* buf = new char[MAX_LINE];
	    LOGGER(5) << "output from command " << cmd;

	    while (fgets(buf, MAX_LINE, fptr))
		LOGGER(5) << buf;

	    delete [] buf;

	    int ierr = pclose(fptr);
	    if (ierr == 0) {
		state = STABLE_STATE;	// end in STABLE_STATE
		if (ibis::gVerbose > 4)
		    logMessage("doBackup", "successfully copied files");
	    }
	    else {
		logWarning("doBackup", "pclose failed ... %s",
			   strerror(errno));
	    }
	}
	else {
	    logError("doBackup", "popen(%s) failed with "
		     "errno = %d", cmd, errno);
	}
	delete [] cmd;
#if (defined(unix) && !defined(__CYGWIN__) && !(defined(sun) && defined(__GNUC__) && __GNUC__ <= 2)) || defined(__HOS_AIX__)
	pthread_sigmask(SIG_UNBLOCK, &sigs, 0);
#endif
    }
} // ibis::part::doBackup

/// Spawn another thread to copy the content of @c activeDir to @c backupDir.
void ibis::part::makeBackupCopy() {
    if (backupDir == 0 || *backupDir == 0 || activeDir == 0)
	return; // nothing to do

    pthread_attr_t tattr;
    int ierr = pthread_attr_init(&tattr);
    if (ierr) {
	logError("makeBackupCopy", "pthread_attr_init failed with %d", ierr);
    }
#if defined(PTHREAD_SCOPE_SYSTEM)
    ierr = pthread_attr_setscope(&tattr, PTHREAD_SCOPE_SYSTEM);
    if (ierr
#if defined(ENOTSUP)
	&& ierr != ENOTSUP
#endif
	) {
	logMessage("makeBackupCopy", "pthread_attr_setscope is unable to "
		   "set system scope (ierr = %d ... %s)", ierr,
		   strerror(ierr));
    }
#endif
#if defined(PTHREAD_CREATE_DETACHED)
    ierr = pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED);
    if (ierr
#if defined(ENOTSUP)
	&& ierr != ENOTSUP
#endif
	) {
	logMessage("makeBackupCopy", "pthread_attr_setdetachstate in unable "
		   "to set detached stat (ierr = %d ... %s)", ierr,
		   strerror(ierr));
    }
#elif defined(unix) || defined(__HOS_AIX__)
    ierr = pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED);
    if (ierr
#if defined(ENOTSUP)
	&& ierr != ENOTSUP
#endif
	) {
	logError("makeBackupCopy", "pthread_attr_setdetachstate is unable to"
		 " set DETACHED state (ierr = %d)", ierr);
    }
#endif

    pthread_t tid;
    ierr = pthread_create(&tid, &tattr, ibis_part_startBackup,
			  (void*)this);
    if (ierr) {
	logError("makeBackupCopy", "pthread_create failed to create a"
		 " detached thread to perform the actual file copying."
		 " returned value is %d", ierr);
    }
    else if (ibis::gVerbose > 1) {
	logMessage("makeBackupCopy", "created a new thread to perform "
		   "the actual copying");
    }
    ierr = pthread_attr_destroy(&tattr);
} // ibis::part::makeBackupCopy

double ibis::part::getActualMin(const char *name) const {
    columnList::const_iterator it = columns.find(name);
    if (it != columns.end())
	return (*it).second->getActualMin();
    else
	return DBL_MAX;
} // ibis::part::getActualMin

double ibis::part::getActualMax(const char *name) const {
    columnList::const_iterator it = columns.find(name);
    if (it != columns.end())
	return (*it).second->getActualMax();
    else
	return -DBL_MAX;
} // ibis::part::getActualMax

double ibis::part::getColumnSum(const char *name) const {
    double ret;
    columnList::const_iterator it = columns.find(name);
    if (it != columns.end())
	ret = (*it).second->getSum();
    else
	ibis::util::setNaN(ret);
    return ret;
} // ibis::part::getColumnSum

// return the number of bins (i.e., counts.size()) on success.
// bin 0: (..., bounds[0]) -> counts[0]
// bin 1: [bounds[0], bounds[1]) -> counts[1]
// bin 2: [bounds[1], bounds[2]) -> counts[2]
// bin 3: [bounds[2], bounds[3]) -> counts[3]
// ...
long
ibis::part::getDistribution(const char *name,
			    std::vector<double>& bounds,
			    std::vector<uint32_t>& counts) const {
    long ierr = -1;
    columnList::const_iterator it = columns.find(name);
    if (it != columns.end()) {
	ierr = (*it).second->getDistribution(bounds, counts);
	if (ierr < 0)
	    ierr -= 10;
    }
    return ierr;
} // ibis::part::getDistribution

/// Because most of the binning scheme leaves two bins for overflow, one
/// for values less than the expected minimum and one for values greater
/// than the expected maximum.  The minimum number of bins expected is
/// four (4).  This function will return error code -1 if the value of nbc
/// is less than 4.
long
ibis::part::getDistribution
(const char *name, uint32_t nbc, double *bounds, uint32_t *counts) const {
    if (nbc < 4) // work space too small
	return -1;
    std::vector<double> bds;
    std::vector<uint32_t> cts;
    long mbc = getDistribution(name, bds, cts);
#if defined(DEBUG) && DEBUG + 0 > 1
    {
	ibis::util::logger lg;
	lg.buffer() << "DEBUG -- getDistribution(" << name
		  << ") returned ierr=" << mbc << ", bds.size()="
		  << bds.size() << ", cts.size()=" << cts.size() << "\n";
	if (mbc > 0 && bds.size()+1 == cts.size() &&
	    static_cast<uint32_t>(mbc) == cts.size()) {
	    lg.buffer() << "(..., " << bds[0] << ")\t" << cts[0] << "\n";
	    for (int i = 1; i < mbc-1; ++ i) {
		lg.buffer() << "[" << bds[i-1] << ", "<< bds[i] << ")\t"
			  << cts[i] << "\n";
	    }
	    lg.buffer() << "[" << bds.back() << ", ...)\t" << cts.back()
		      << "\n";
	}
    }
#endif
    mbc = packDistribution(bds, cts, nbc, bounds, counts);
    return mbc;
} // ibis::part::getDistribution

long ibis::part::get1DDistribution(const char *constraints, const char *cname,
				   double begin, double end, double stride,
				   std::vector<size_t>& counts) const {
    if (cname == 0 || *cname == 0 || (begin >= end && stride >= 0.0) ||
	(begin <= end && stride <= 0.0))
	return -1L;

    const ibis::column* col = getColumn(cname);
    if (col == 0)
	return -2L;

    const size_t nbins = 1 + 
	static_cast<size_t>(std::floor((end - begin) / stride));
    if (counts.size() != nbins) {
	counts.resize(nbins);
	for (size_t i = 0; i < nbins; ++i)
	    counts[i] = 0;
    }

    ibis::query qq(ibis::util::userName(), this);
    qq.setSelectClause(cname);
    {
	std::ostringstream oss;
	if (constraints != 0 && *constraints != 0)
	    oss << "(" << constraints << ") AND ";
	oss << cname << " between " << begin << " and " << end;
	qq.setWhereClause(oss.str().c_str());
    }

    long ierr = qq.evaluate();
    if (ierr < 0)
	return ierr;
    ierr = nbins;
    if (qq.getNumHits() == 0)
	return ierr;

    switch (col->type()) {
    case ibis::BYTE:
    case ibis::SHORT:
    case ibis::INT: {
	array_t<int32_t>* vals = col->selectInts(*(qq.getHitVector()));
	if (vals != 0) {
	    for (size_t i = 0; i < vals->size(); ++ i) {
		++ counts[static_cast<size_t>
			  (std::floor(((*vals)[i] - begin) / stride))];
	    }
	    delete vals;
	}
	else {
	    ierr = -4;
	}
	break;}
    case ibis::UBYTE:
    case ibis::USHORT:
    case ibis::UINT: {
	array_t<uint32_t>* vals = col->selectUInts(*(qq.getHitVector()));
	if (vals != 0) {
	    for (size_t i = 0; i < vals->size(); ++ i) {
		++ counts[static_cast<size_t>
			  (std::floor(((*vals)[i] - begin) / stride))];
	    }
	    delete vals;
	}
	else {
	    ierr = -4;
	}
	break;}
    case ibis::ULONG:
    case ibis::LONG: {
	array_t<int64_t>* vals = col->selectLongs(*(qq.getHitVector()));
	if (vals != 0) {
	    for (size_t i = 0; i < vals->size(); ++ i) {
		++ counts[static_cast<size_t>
			  (std::floor(((*vals)[i] - begin) / stride))];
	    }
	    delete vals;
	}
	else {
	    ierr = -4;
	}
	break;}
    case ibis::FLOAT: {
	array_t<float>* vals = col->selectFloats(*(qq.getHitVector()));
	if (vals != 0) {
	    for (size_t i = 0; i < vals->size(); ++ i) {
		++ counts[static_cast<size_t>
			  (std::floor(((*vals)[i] - begin) / stride))];
	    }
	    delete vals;
	}
	else {
	    ierr = -4;
	}
	break;}
    case ibis::DOUBLE: {
	array_t<double>* vals = col->selectDoubles(*(qq.getHitVector()));
	if (vals != 0) {
	    for (size_t i = 0; i < vals->size(); ++ i) {
		++ counts[static_cast<size_t>
			  (std::floor(((*vals)[i] - begin) / stride))];
	    }
	    delete vals;
	}
	else {
	    ierr = -4;
	}
	break;}
    default: {
	LOGGER(4)
	    << "ibis::part::get1DDistribution -- unable to "
	    "handle column (" << cname << ") type "
	    << ibis::TYPESTRING[(int)col->type()];

	ierr = -3;
	break;}
    }
    return ierr;
} // ibis::part::get1DDistribution

template <typename T1, typename T2>
long ibis::part::count2DBins(const array_t<T1>& vals1,
			     const double& begin1, const double& end1,
			     const double& stride1,
			     const array_t<T2>& vals2,
			     const double& begin2, const double& end2,
			     const double& stride2,
			     std::vector<size_t>& counts) const {
    const size_t dim2 = 1+
	static_cast<size_t>(std::floor((end2-begin2)/stride2));
    for (size_t i1 = 0; i1 < vals1.size(); ++ i1) {
	size_t j1 = dim2 *
	    static_cast<size_t>(std::floor((vals1[i1]-begin1)/stride1));
	for (size_t i2 = 0; i2 < vals2.size(); ++ i2)
	    ++ counts[j1+static_cast<size_t>
		      (std::floor((vals2[i2]-begin2)/stride2))];
    }
    return counts.size();
} // ibis::part::count2DBins

template <typename T1, typename T2, typename T3>
long ibis::part::count3DBins(const array_t<T1>& vals1,
			     const double& begin1, const double& end1,
			     const double& stride1,
			     const array_t<T2>& vals2,
			     const double& begin2, const double& end2,
			     const double& stride2,
			     const array_t<T3>& vals3,
			     const double& begin3, const double& end3,
			     const double& stride3,
			     std::vector<size_t>& counts) const {
    const size_t dim3 = 1 +
	static_cast<size_t>(std::floor((end3 - begin3)/stride3));
    const size_t dim2 = dim3 + dim3 *
	static_cast<size_t>(std::floor((end2 - begin2)/stride2));
    for (size_t i1 = 0; i1 < vals1.size(); ++ i1) {
	size_t j1 = dim2 *
	    static_cast<size_t>(std::floor((vals1[i1]-begin1)/stride1));
	for (size_t i2 = 0; i2 < vals2.size(); ++ i2) {
	    size_t j2 = j1 + dim3 *
		static_cast<size_t>(std::floor((vals2[i2]-begin2)/stride2));
	    for (size_t i3 = 0; i3 < vals3.size(); ++ i3)
		++ counts[j2 + static_cast<size_t>
			  (std::floor((vals3[i3]-begin3)/stride3))];
	}
    }
    return counts.size();
} // ibis::part::count3DBins

long ibis::part::get2DDistribution(const char *constraints, const char *cname1,
				   double begin1, double end1, double stride1,
				   const char *cname2,
				   double begin2, double end2, double stride2,
				   std::vector<size_t>& counts) const {
    if (cname1 == 0 || *cname1 == 0 || (begin1 >= end1 && stride1 >= 0.0) ||
	(begin1 <= end1 && stride1 <= 0.0) ||
	cname2 == 0 || *cname2 == 0 || (begin2 >= end2 && stride2 >= 0.0) ||
	(begin2 <= end2 && stride2 <= 0.0))
	return -1L;

    const ibis::column* col1 = getColumn(cname1);
    const ibis::column* col2 = getColumn(cname2);
    if (col1 == 0 || col2 == 0)
	return -2L;

    const size_t nbins =
	(1 + static_cast<size_t>(std::floor((end1 - begin1) / stride1))) *
	(1 + static_cast<size_t>(std::floor((end2 - begin2) / stride2)));
    if (counts.size() != nbins) {
	counts.resize(nbins);
	for (size_t i = 0; i < nbins; ++i)
	    counts[i] = 0;
    }

    ibis::query qq(ibis::util::userName(), this);
    {
	std::string sel = cname1;
	sel += ',';
	sel += cname2;
	qq.setSelectClause(sel.c_str());
    }
    { // add constraints on the two selected variables
	std::ostringstream oss;
	if (constraints != 0 && *constraints != 0)
	    oss << "(" << constraints << ") AND ";
	oss << cname1 << " between " << begin1 << " and " << end1
	    << " AND " << cname2 << " between " << begin2 << " and "
	    << end2;
	qq.setWhereClause(oss.str().c_str());
    }

    long ierr = qq.evaluate();
    if (ierr < 0)
	return ierr;
    ierr = nbins;
    if (qq.getNumHits() == 0)
	return ierr;

    switch (col1->type()) {
    case ibis::BYTE:
    case ibis::SHORT:
    case ibis::INT: {
	array_t<int32_t>* vals1 = col1->selectInts(*(qq.getHitVector()));
	if (vals1 == 0) {
	    ierr = -4;
	    break;
	}

	switch (col2->type()) {
	case ibis::BYTE:
	case ibis::SHORT:
	case ibis::INT: {
	    array_t<int32_t>* vals2 = col2->selectInts(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	case ibis::UBYTE:
	case ibis::USHORT:
	case ibis::UINT: {
	    array_t<uint32_t>* vals2 = col2->selectUInts(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	case ibis::ULONG:
	case ibis::LONG: {
	    array_t<int64_t>* vals2 = col2->selectLongs(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	case ibis::FLOAT: {
	    array_t<float>* vals2 = col2->selectFloats(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	case ibis::DOUBLE: {
	    array_t<double>* vals2 = col2->selectDoubles(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	default: {
	    LOGGER(4)
		<< "ibis::part::get2DDistribution -- unable to "
		"handle column (" << cname2 << ") type "
		<< ibis::TYPESTRING[(int)col2->type()];

	    ierr = -3;
	    break;}
	}
	delete vals1;
	break;}
    case ibis::UBYTE:
    case ibis::USHORT:
    case ibis::UINT: {
	array_t<uint32_t>* vals1 = col1->selectUInts(*(qq.getHitVector()));
	if (vals1 == 0) {
	    ierr = -4;
	    break;
	}

	switch (col2->type()) {
	case ibis::BYTE:
	case ibis::SHORT:
	case ibis::INT: {
	    array_t<int32_t>* vals2 = col2->selectInts(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	case ibis::UBYTE:
	case ibis::USHORT:
	case ibis::UINT: {
	    array_t<uint32_t>* vals2 = col2->selectUInts(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	case ibis::ULONG:
	case ibis::LONG: {
	    array_t<int64_t>* vals2 = col2->selectLongs(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	case ibis::FLOAT: {
	    array_t<float>* vals2 = col2->selectFloats(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	case ibis::DOUBLE: {
	    array_t<double>* vals2 = col2->selectDoubles(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	default: {
	    LOGGER(4)
		<< "ibis::part::get2DDistribution -- unable to "
		"handle column (" << cname2 << ") type "
		<< ibis::TYPESTRING[(int)col2->type()];

	    ierr = -3;
	    break;}
	}
	delete vals1;
	break;}
    case ibis::ULONG:
    case ibis::LONG: {
	array_t<int64_t>* vals1 = col1->selectLongs(*(qq.getHitVector()));
	if (vals1 == 0) {
	    ierr = -4;
	    break;
	}

	switch (col2->type()) {
	case ibis::BYTE:
	case ibis::SHORT:
	case ibis::INT: {
	    array_t<int32_t>* vals2 = col2->selectInts(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	case ibis::UBYTE:
	case ibis::USHORT:
	case ibis::UINT: {
	    array_t<uint32_t>* vals2 = col2->selectUInts(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	case ibis::ULONG:
	case ibis::LONG: {
	    array_t<int64_t>* vals2 = col2->selectLongs(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	case ibis::FLOAT: {
	    array_t<float>* vals2 = col2->selectFloats(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	case ibis::DOUBLE: {
	    array_t<double>* vals2 = col2->selectDoubles(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	default: {
	    LOGGER(4)
		<< "ibis::part::get2DDistribution -- unable to "
		"handle column (" << cname2 << ") type "
		<< ibis::TYPESTRING[(int)col2->type()];

	    ierr = -3;
	    break;}
	}
	delete vals1;
	break;}
    case ibis::FLOAT: {
	array_t<float>* vals1 = col1->selectFloats(*(qq.getHitVector()));
	if (vals1 == 0) {
	    ierr = -4;
	    break;
	}

	switch (col2->type()) {
	case ibis::BYTE:
	case ibis::SHORT:
	case ibis::INT: {
	    array_t<int32_t>* vals2 = col2->selectInts(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	case ibis::UBYTE:
	case ibis::USHORT:
	case ibis::UINT: {
	    array_t<uint32_t>* vals2 = col2->selectUInts(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	case ibis::ULONG:
	case ibis::LONG: {
	    array_t<int64_t>* vals2 = col2->selectLongs(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	case ibis::FLOAT: {
	    array_t<float>* vals2 = col2->selectFloats(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	case ibis::DOUBLE: {
	    array_t<double>* vals2 = col2->selectDoubles(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	default: {
	    LOGGER(4)
		<< "ibis::part::get2DDistribution -- unable to "
		"handle column (" << cname2 << ") type "
		<< ibis::TYPESTRING[(int)col2->type()];

	    ierr = -3;
	    break;}
	}
	delete vals1;
	break;}
    case ibis::DOUBLE: {
	array_t<double>* vals1 = col1->selectDoubles(*(qq.getHitVector()));
	if (vals1 == 0) {
	    ierr = -4;
	    break;
	}

	switch (col2->type()) {
	case ibis::BYTE:
	case ibis::SHORT:
	case ibis::INT: {
	    array_t<int32_t>* vals2 = col2->selectInts(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	case ibis::UBYTE:
	case ibis::USHORT:
	case ibis::UINT: {
	    array_t<uint32_t>* vals2 = col2->selectUInts(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	case ibis::ULONG:
	case ibis::LONG: {
	    array_t<int64_t>* vals2 = col2->selectLongs(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	case ibis::FLOAT: {
	    array_t<float>* vals2 = col2->selectFloats(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	case ibis::DOUBLE: {
	    array_t<double>* vals2 = col2->selectDoubles(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }
	    ierr = count2DBins(*vals1, begin1, end1, stride1,
			       *vals2, begin2, end2, stride2, counts);
	    delete vals2;
	    break;}
	default: {
	    LOGGER(4)
		<< "ibis::part::get2DDistribution -- unable to "
		"handle column (" << cname2 << ") type "
		<< ibis::TYPESTRING[(int)col2->type()];

	    ierr = -3;
	    break;}
	}
	delete vals1;
	break;}
    default: {
	LOGGER(4)
	    << "ibis::part::get2DDistribution -- unable to "
	    "handle column (" << cname1 << ") type "
	    << ibis::TYPESTRING[(int)col1->type()];

	ierr = -3;
	break;}
    }
    return ierr;
} // ibis::part::get2DDistribution

long ibis::part::get3DDistribution(const char *constraints, const char *cname1,
				   double begin1, double end1, double stride1,
				   const char *cname2,
				   double begin2, double end2, double stride2,
				   const char *cname3,
				   double begin3, double end3, double stride3,
				   std::vector<size_t>& counts) const {
    if (cname1 == 0 || *cname1 == 0 || (begin1 >= end1 && stride1 >= 0.0) ||
	(begin1 <= end1 && stride1 <= 0.0) ||
	cname2 == 0 || *cname2 == 0 || (begin2 >= end2 && stride2 >= 0.0) ||
	(begin2 <= end2 && stride2 <= 0.0) ||
	cname3 == 0 || *cname3 == 0 || (begin3 >= end3 && stride3 >= 0.0) ||
	(begin3 <= end3 && stride3 <= 0.0))
	return -1L;

    const ibis::column* col1 = getColumn(cname1);
    const ibis::column* col2 = getColumn(cname2);
    const ibis::column* col3 = getColumn(cname3);
    if (col1 == 0 || col2 == 0 || col3 == 0)
	return -2L;

    const size_t nbins =
	(1 + static_cast<size_t>(std::floor((end1 - begin1) / stride1))) *
	(1 + static_cast<size_t>(std::floor((end2 - begin2) / stride2))) *
	(1 + static_cast<size_t>(std::floor((end3 - begin3) / stride3)));
    if (counts.size() != nbins) {
	counts.resize(nbins);
	for (size_t i = 0; i < nbins; ++i)
	    counts[i] = 0;
    }

    ibis::query qq(ibis::util::userName(), this);
    {
	std::string sel = cname1;
	sel += ',';
	sel += cname2;
	sel += ',';
	sel += cname3;
	qq.setSelectClause(sel.c_str());
    }
    { // add constraints on the two selected variables
	std::ostringstream oss;
	if (constraints != 0 && *constraints != 0)
	    oss << "(" << constraints << ") AND ";
	oss << cname1 << " between " << begin1 << " and " << end1
	    << " AND " << cname2 << " between " << begin2 << " and "
	    << end2
	    << " AND " << cname3 << " between " << begin3 << " and "
	    << end3;
	qq.setWhereClause(oss.str().c_str());
    }

    long ierr = qq.evaluate();
    if (ierr < 0)
	return ierr;
    ierr = nbins;
    if (qq.getNumHits() == 0)
	return ierr;

    switch (col1->type()) {
    case ibis::BYTE:
    case ibis::SHORT:
    case ibis::INT: {
	array_t<int32_t>* vals1 = col1->selectInts(*(qq.getHitVector()));
	if (vals1 == 0) {
	    ierr = -4;
	    break;
	}

	switch (col2->type()) {
	case ibis::BYTE:
	case ibis::SHORT:
	case ibis::INT: {
	    array_t<int32_t>* vals2 = col2->selectInts(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	case ibis::UBYTE:
	case ibis::USHORT:
	case ibis::UINT: {
	    array_t<uint32_t>* vals2 = col2->selectUInts(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	case ibis::ULONG:
	case ibis::LONG: {
	    array_t<int64_t>* vals2 = col2->selectLongs(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	case ibis::FLOAT: {
	    array_t<float>* vals2 = col2->selectFloats(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	case ibis::DOUBLE: {
	    array_t<double>* vals2 = col2->selectDoubles(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	default: {
	    LOGGER(4)
		<< "ibis::part::get3DDistribution -- unable to "
		"handle column (" << cname2 << ") type "
		<< ibis::TYPESTRING[(int)col2->type()];

	    ierr = -3;
	    break;}
	}
	delete vals1;
	break;}
    case ibis::UBYTE:
    case ibis::USHORT:
    case ibis::UINT: {
	array_t<uint32_t>* vals1 = col1->selectUInts(*(qq.getHitVector()));
	if (vals1 == 0) {
	    ierr = -4;
	    break;
	}

	switch (col2->type()) {
	case ibis::BYTE:
	case ibis::SHORT:
	case ibis::INT: {
	    array_t<int32_t>* vals2 = col2->selectInts(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	case ibis::UBYTE:
	case ibis::USHORT:
	case ibis::UINT: {
	    array_t<uint32_t>* vals2 = col2->selectUInts(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	case ibis::ULONG:
	case ibis::LONG: {
	    array_t<int64_t>* vals2 = col2->selectLongs(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	case ibis::FLOAT: {
	    array_t<float>* vals2 = col2->selectFloats(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	case ibis::DOUBLE: {
	    array_t<double>* vals2 = col2->selectDoubles(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	default: {
	    LOGGER(4)
		<< "ibis::part::get3DDistribution -- unable to "
		"handle column (" << cname2 << ") type "
		<< ibis::TYPESTRING[(int)col2->type()];

	    ierr = -3;
	    break;}
	}
	delete vals1;
	break;}
    case ibis::ULONG:
    case ibis::LONG: {
	array_t<int64_t>* vals1 = col1->selectLongs(*(qq.getHitVector()));
	if (vals1 == 0) {
	    ierr = -4;
	    break;
	}

	switch (col2->type()) {
	case ibis::BYTE:
	case ibis::SHORT:
	case ibis::INT: {
	    array_t<int32_t>* vals2 = col2->selectInts(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	case ibis::UBYTE:
	case ibis::USHORT:
	case ibis::UINT: {
	    array_t<uint32_t>* vals2 = col2->selectUInts(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	case ibis::ULONG:
	case ibis::LONG: {
	    array_t<int64_t>* vals2 = col2->selectLongs(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	case ibis::FLOAT: {
	    array_t<float>* vals2 = col2->selectFloats(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	case ibis::DOUBLE: {
	    array_t<double>* vals2 = col2->selectDoubles(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	default: {
	    LOGGER(4)
		<< "ibis::part::get3DDistribution -- unable to "
		"handle column (" << cname2 << ") type "
		<< ibis::TYPESTRING[(int)col2->type()];

	    ierr = -3;
	    break;}
	}
	delete vals1;
	break;}
    case ibis::FLOAT: {
	array_t<float>* vals1 = col1->selectFloats(*(qq.getHitVector()));
	if (vals1 == 0) {
	    ierr = -4;
	    break;
	}

	switch (col2->type()) {
	case ibis::BYTE:
	case ibis::SHORT:
	case ibis::INT: {
	    array_t<int32_t>* vals2 = col2->selectInts(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	case ibis::UBYTE:
	case ibis::USHORT:
	case ibis::UINT: {
	    array_t<uint32_t>* vals2 = col2->selectUInts(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	case ibis::ULONG:
	case ibis::LONG: {
	    array_t<int64_t>* vals2 = col2->selectLongs(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	case ibis::FLOAT: {
	    array_t<float>* vals2 = col2->selectFloats(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	case ibis::DOUBLE: {
	    array_t<double>* vals2 = col2->selectDoubles(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	default: {
	    LOGGER(4)
		<< "ibis::part::get3DDistribution -- unable to "
		"handle column (" << cname2 << ") type "
		<< ibis::TYPESTRING[(int)col2->type()];

	    ierr = -3;
	    break;}
	}
	delete vals1;
	break;}
    case ibis::DOUBLE: {
	array_t<double>* vals1 = col1->selectDoubles(*(qq.getHitVector()));
	if (vals1 == 0) {
	    ierr = -4;
	    break;
	}

	switch (col2->type()) {
	case ibis::BYTE:
	case ibis::SHORT:
	case ibis::INT: {
	    array_t<int32_t>* vals2 = col2->selectInts(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	case ibis::UBYTE:
	case ibis::USHORT:
	case ibis::UINT: {
	    array_t<uint32_t>* vals2 = col2->selectUInts(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	case ibis::ULONG:
	case ibis::LONG: {
	    array_t<int64_t>* vals2 = col2->selectLongs(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	case ibis::FLOAT: {
	    array_t<float>* vals2 = col2->selectFloats(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	case ibis::DOUBLE: {
	    array_t<double>* vals2 = col2->selectDoubles(*(qq.getHitVector()));
	    if (vals2 == 0) {
		ierr = -5;
		break;
	    }

	    switch (col3->type()) {
	    case ibis::BYTE:
	    case ibis::SHORT:
	    case ibis::INT: {
		array_t<int32_t>* vals3 =
		    col3->selectInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::UBYTE:
	    case ibis::USHORT:
	    case ibis::UINT: {
		array_t<uint32_t>* vals3 =
		    col3->selectUInts(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::ULONG:
	    case ibis::LONG: {
		array_t<int64_t>* vals3 =
		    col3->selectLongs(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end2, stride3, counts);
		delete vals3;
		break;}
	    case ibis::FLOAT: {
		array_t<float>* vals3 =
		    col3->selectFloats(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals3, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    case ibis::DOUBLE: {
		array_t<double>* vals3 =
		    col3->selectDoubles(*(qq.getHitVector()));
		if (vals3 == 0) {
		    ierr = -6;
		    break;
		}
		ierr = count3DBins(*vals1, begin1, end1, stride1,
				   *vals2, begin2, end2, stride2,
				   *vals2, begin3, end3, stride3, counts);
		delete vals3;
		break;}
	    default: {
		LOGGER(4)
		    << "ibis::part::get3DDistribution -- unable to "
		    "handle column (" << cname3 << ") type "
		    << ibis::TYPESTRING[(int)col3->type()];

		ierr = -3;
		break;}
	    }
	    delete vals2;
	    break;}
	default: {
	    LOGGER(4)
		<< "ibis::part::get3DDistribution -- unable to "
		"handle column (" << cname2 << ") type "
		<< ibis::TYPESTRING[(int)col2->type()];

	    ierr = -3;
	    break;}
	}
	delete vals1;
	break;}
    default: {
	LOGGER(4)
	    << "ibis::part::get3DDistribution -- unable to "
	    "handle column (" << cname1 << ") type "
	    << ibis::TYPESTRING[(int)col1->type()];

	ierr = -3;
	break;}
    }
    return ierr;
} // ibis::part::get3DDistribution

/// Compute the distribution of the named variable under the specified
/// constraints.  If the input array @c bounds contains distinct values in
/// ascending order, the array will be used as bin boundaries.  Otherwise,
/// the bin boundaries are automatically determined by this function.  The
/// basic rule for determining the number of bins is that if there are less
/// than 10,000 distinct values, than everyone value is counted separatly,
/// otherwise 1000 bins will be used and each bin will contain roughly the
/// same number of records.
long
ibis::part::getDistribution(const char *constraints,
			    const char *name,
			    std::vector<double>& bounds,
			    std::vector<uint32_t>& counts) const {
    long ierr = -1;
    columnList::const_iterator it = columns.find(name);
    if (it == columns.end())
	return ierr;

    ibis::horometer timer;
    if (ibis::gVerbose > 2)
	timer.start();
//     if (constraints == 0 || *constraints == 0) {
// 	ierr = (*it).second->getDistribution(bounds, counts);
// 	if (ierr > 0 && ibis::gVerbose > 2) {
// 	    timer.stop();
// 	    logMessage("getDistribution",
// 		       "computing the distribution of column %s took %g "
// 		       "sec(CPU) and %g sec(elapsed)",
// 		       (*it).first, timer.CPUTime(), timer.realTime());
// 	}
// 	return ierr;
//     }

    ibis::bitvector mask;
    mask.set(1, nEvents);
    const ibis::column *col = (*it).second;
    if (constraints != 0 && *constraints != 0) {
	ibis::query q(ibis::util::userName(), this);
	q.setWhereClause(constraints);
	ierr = q.evaluate();
	if (ierr < 0) {
	    ierr = -2;
	    return ierr;
	}

	mask.copy(*(q.getHitVector()));
	if (mask.cnt() == 0) {
	    if (ibis::gVerbose > 2)
		logMessage("getDistribution", "no record satisfied the "
			   "user specified constraints \"%s\"", constraints);
	    return 0;
	}
    }
    bool usebnds = ! bounds.empty();
    for (uint32_t i = 1; usebnds && i < bounds.size(); ++ i)
	usebnds = (bounds[i] > bounds[i-1]);

    if (usebnds) { // use the input bin boundaries
	switch ((*it).second->type()) {
	case ibis::SHORT:
	case ibis::BYTE:
	case ibis::INT: {
	    array_t<int32_t> *vals = col->selectInts(mask);
	    if (vals == 0) {
		ierr = -4;
		break;
	    }
	    array_t<int32_t> bnds(bounds.size());
	    for (uint32_t i = 0; i < bounds.size(); ++ i)
		bnds[i] = static_cast<int32_t>(bounds[i]);
	    ibis::index::mapValues<int32_t>(*vals, bnds, counts);
	    delete vals;
	    break;}
	case ibis::USHORT:
	case ibis::UBYTE:
	case ibis::UINT:
	case ibis::CATEGORY: {
	    array_t<uint32_t> *vals = col->selectUInts(mask);
	    if (vals == 0) {
		ierr = -4;
		break;
	    }
	    array_t<uint32_t> bnds(bounds.size());
	    for (uint32_t i = 0; i < bounds.size(); ++ i)
		bnds[i] = static_cast<uint32_t>(bounds[i]);
	    ibis::index::mapValues<uint32_t>(*vals, bnds, counts);
	    delete vals;
	    break;}
	case ibis::FLOAT: {
	    array_t<float> *vals = col->selectFloats(mask);
	    if (vals == 0) {
		ierr = -4;
		break;
	    }
	    array_t<float> bnds(bounds.size());
	    for (uint32_t i = 0; i < bounds.size(); ++ i)
		bnds[i] = static_cast<float>(bounds[i]);
	    ibis::index::mapValues<float>(*vals, bnds, counts);
	    delete vals;
	    break;}
	case ibis::DOUBLE: {
	    array_t<double> *vals = col->selectDoubles(mask);
	    if (vals == 0) {
		ierr = -4;
		break;
	    }
	    array_t<double> bnds(bounds.size());
	    for (uint32_t i = 0; i < bounds.size(); ++ i)
		bnds[i] = bounds[i];
	    ibis::index::mapValues<double>(*vals, bnds, counts);
	    delete vals;
	    break;}
	default: {
	    ierr = -3;
	    logWarning("getDistribution",
		       "unable to handle column type %d",
		       static_cast<int>((*it).second->type()));
	    break;}
	}
    }
    else { // need to determine bin boundaries in this function
	ibis::index::histogram hist;
	bounds.clear();
	counts.clear();
	switch ((*it).second->type()) {
	case ibis::SHORT:
	case ibis::BYTE:
	case ibis::INT: {
	    array_t<int32_t> *vals = col->selectInts(mask);
	    if (vals == 0) {
		ierr = -4;
		break;
	    }
	    ibis::index::mapValues<int32_t>(*vals, hist);
	    delete vals;
	    break;}
	case ibis::USHORT:
	case ibis::UBYTE:
	case ibis::UINT:
	case ibis::CATEGORY: {
	    array_t<uint32_t> *vals = col->selectUInts(mask);
	    if (vals == 0) {
		ierr = -4;
		break;
	    }
	    ibis::index::mapValues<uint32_t>(*vals, hist);
	    delete vals;
	    break;}
	case ibis::FLOAT: {
	    array_t<float> *vals = col->selectFloats(mask);
	    if (vals == 0) {
		ierr = -4;
		break;
	    }
	    ibis::index::mapValues<float>(*vals, hist);
	    delete vals;
	    break;}
	case ibis::DOUBLE: {
	    array_t<double> *vals = col->selectDoubles(mask);
	    if (vals == 0) {
		ierr = -4;
		break;
	    }
	    ibis::index::mapValues<double>(*vals, hist);
	    delete vals;
	    break;}
	default: {
	    ierr = -3;
	    logWarning("getDistribution",
		       "unable to handle column type %d",
		       static_cast<int>((*it).second->type()));
	    break;}
	}

	if (hist.size() == 1) { // special case of a single value
	    ibis::index::histogram::const_iterator it1 =
		hist.begin();
	    bounds.resize(2);
	    counts.resize(3);
	    bounds[0] = (*it1).first;
	    bounds[1] = ((*it1).first + 1.0);
	    counts[0] = 0;
	    counts[1] = (*it1).second;
	    counts[2] = 0;
	}
	else if (hist.size() < 10000) {
	    // convert the histogram into two arrays
	    bounds.reserve(mask.cnt());
	    counts.reserve(mask.cnt()+1);
	    ibis::index::histogram::const_iterator it1 =
		hist.begin();
	    counts.push_back((*it1).second);
	    for (++ it1; it1 != hist.end(); ++ it1) {
		bounds.push_back((*it1).first);
		counts.push_back((*it1).second);
	    }
	}
	else if (hist.size() > 0) { // too many values, reduce to 1000 bins
	    array_t<double> vals(hist.size());
	    array_t<uint32_t> cnts(hist.size());
	    vals.clear();
	    cnts.clear();
	    for (ibis::index::histogram::const_iterator it1 =
		     hist.begin();
		 it1 != hist.end(); ++ it1) {
		vals.push_back((*it1).first);
		cnts.push_back((*it1).second);
	    }
	    array_t<uint32_t> dvd(1000);
	    ibis::index::divideCounts(dvd, cnts);
	    for (uint32_t i = 0; i < dvd.size(); ++ i) {
		uint32_t cnt = 0;
		for (uint32_t j = (i>0?dvd[i-1]:0); j < dvd[i]; ++ j)
		    cnt += cnts[j];
		counts.push_back(cnt);
		if (i > 0) {
		    double bd;
		    if (dvd[i] < vals.size())
			bd = ibis::util::compactValue
			    (vals[dvd[i]-1], vals[dvd[i]]);
		    else
			bd = ibis::util::compactValue
			    (vals.back(), DBL_MAX);
		    bounds.push_back(bd);
		}
	    }
	}
    }
    if (ierr >= 0)
	ierr = counts.size();
    if (ierr > 0 && ibis::gVerbose > 2) {
	timer.stop();
	logMessage("getDistribution",
		   "computing the distribution of "
		   "column %s with restriction \"%s\" took %g "
		   "sec(CPU) and %g sec(elapsed)", (*it).first,
		   constraints, timer.CPUTime(), timer.realTime());
    }
    if (ierr < 0)
	ierr -= 10;
    return ierr;
} // ibis::part::getDistribution

/// Because most of the binning scheme leaves two bins for overflow, one
/// for values less than the expected minimum and one for values greater
/// than the expected maximum.  The minimum number of bins expected is
/// four (4).  This function will return error code -1 if the value of nbc
/// is less than 4.
long
ibis::part::getDistribution
(const char *constraints, const char *name, uint32_t nbc,
 double *bounds, uint32_t *counts) const {
    if (nbc < 4) // work space too small
	return -1;

    std::vector<double> bds;
    std::vector<uint32_t> cts;
    bool useinput = true;
    for (uint32_t i = 1; i < nbc && useinput; ++ i)
	useinput = (bounds[i] > bounds[i-1]);
    if (useinput) {
	bds.resize(nbc);
	for (uint32_t i = 0; i < nbc; ++i)
	    bds[i] = bounds[i];
    }
    long mbc = getDistribution(constraints, name, bds, cts);
#if defined(DEBUG) && DEBUG + 0 > 1
    {
	ibis::util::logger lg;
	lg.buffer() << "DEBUG -- getDistribution(" << name << ", "
		    << constraints << ") returned ierr=" << mbc
		    << ", bds.size()=" << bds.size() << ", cts.size()="
		    << cts.size() << "\n";
	if (mbc > 0 && bds.size()+1 == cts.size() &&
	    static_cast<uint32_t>(mbc) == cts.size()) {
	    lg.buffer() << "(..., " << bds[0] << ")\t" << cts[0] << "\n";
	    for (int i = 1; i < mbc-1; ++ i) {
		lg.buffer() << "[" << bds[i-1] << ", "<< bds[i] << ")\t"
			  << cts[i] << "\n";
	    }
	    lg.buffer() << "[" << bds.back() << ", ...)\t" << cts.back()
		      << "\n";
	}
    }
#endif
    mbc = packDistribution(bds, cts, nbc, bounds, counts);
    return mbc;
} // ibis::part::getDistribution

long
ibis::part::getCumulativeDistribution(const char *name,
				      std::vector<double>& bounds,
				      std::vector<uint32_t>& counts) const {
    long ierr = -1;
    columnList::const_iterator it = columns.find(name);
    if (it != columns.end()) {
	ierr = (*it).second->getCumulativeDistribution(bounds, counts);
	if (ierr < 0)
	    ierr -= 10;
    }
    return ierr;
} // ibis::part::getCumulativeDistribution

/// Because most of the binning scheme leaves two bins for overflow, one
/// for values less than the expected minimum and one for values greater
/// than the expected maximum.  The minimum number of bins expected is
/// four (4).  This function will return error code -1 if the value of nbc
/// is less than 4.
long
ibis::part::getCumulativeDistribution
(const char *name, uint32_t nbc, double *bounds, uint32_t *counts) const {
    if (nbc < 4) // work space too small
	return -1;
    std::vector<double> bds;
    std::vector<uint32_t> cts;
    long mbc = getCumulativeDistribution(name, bds, cts);
#if defined(DEBUG) && DEBUG + 0 > 1
    {
	ibis::util::logger lg;
	lg.buffer() << "DEBUG -- getCumulativeDistribution(" << name
		  << ") returned ierr=" << mbc << "\n";
	if (mbc > 0)
	    lg.buffer() << "histogram\n(bound,\tcount)\n";
	for (int i = 0; i < mbc; ++ i) {
	    lg.buffer() << bds[i] << ",\t" << cts[i] << "\n";
	}
    }
#endif
    mbc = packCumulativeDistribution(bds, cts, nbc, bounds, counts);
    return mbc;
} // ibis::part::getCumulativeDistribution

/// @note This function does not accept user input bin boundaries.
long
ibis::part::getCumulativeDistribution(const char *constraints,
				      const char *name,
				      std::vector<double>& bounds,
				      std::vector<uint32_t>& counts) const {
    long ierr = -1;
    columnList::const_iterator it = columns.find(name);
    if (it != columns.end()) {
	ibis::horometer timer;
	if (ibis::gVerbose > 2)
	    timer.start();
	if (constraints == 0 || *constraints == 0) {
	    ierr = (*it).second->getCumulativeDistribution(bounds, counts);
	    if (ierr > 0 && ibis::gVerbose > 2) {
		timer.stop();
		logMessage("getCumulativeDistribution",
			   "computing the distribution of column %s took %g "
			   "sec(CPU) and %g sec(elapsed)",
			   (*it).first, timer.CPUTime(), timer.realTime());
	    }
	}
	else {
	    const ibis::column *col = (*it).second;
	    ibis::query q(ibis::util::userName(), this);
	    q.setWhereClause(constraints);
	    ierr = q.evaluate();
	    if (ierr < 0)
		return ierr;
	    const ibis::bitvector *hits = q.getHitVector();
	    ibis::index::histogram hist;
	    bounds.clear();
	    counts.clear();
	    if (hits != 0 && hits->cnt() > 0) {
		switch ((*it).second->type()) {
		case ibis::SHORT:
		case ibis::BYTE:
		case ibis::INT: {
		    array_t<int32_t> *vals = col->selectInts(*hits);
		    if (vals == 0) {
			ierr = -4;
			break;
		    }
		    ibis::index::mapValues<int32_t>(*vals, hist);
		    delete vals;
		    break;}
		case ibis::USHORT:
		case ibis::UBYTE:
		case ibis::UINT:
		case ibis::CATEGORY: {
		    array_t<uint32_t> *vals = col->selectUInts(*hits);
		    if (vals == 0) {
			ierr = -4;
			break;
		    }
		    ibis::index::mapValues<uint32_t>(*vals, hist);
		    delete vals;
		    break;}
		case ibis::FLOAT: {
		    array_t<float> *vals = col->selectFloats(*hits);
		    if (vals == 0) {
			ierr = -4;
			break;
		    }
		    ibis::index::mapValues<float>(*vals, hist);
		    delete vals;
		    break;}
		case ibis::DOUBLE: {
		    array_t<double> *vals = col->selectDoubles(*hits);
		    if (vals == 0) {
			ierr = -4;
			break;
		    }
		    ibis::index::mapValues<double>(*vals, hist);
		    delete vals;
		    break;}
		default: {
		    ierr = -3;
		    logWarning("getCumulativeDistribution",
			       "unable to handle column type %d",
			       static_cast<int>((*it).second->type()));
		    break;}
		}

		if (hist.empty()) {
		    if (ierr >= 0)
			ierr = -7;
		}
		else if (hist.size() < 10000) {
		    // convert the histogram into cumulative distribution
		    bounds.reserve(hits->cnt()+1);
		    counts.reserve(hits->cnt()+1);
		    counts.push_back(0);
		    for (ibis::index::histogram::const_iterator hit =
			     hist.begin();
			 hit != hist.end(); ++ hit) {
			bounds.push_back((*hit).first);
			counts.push_back((*hit).second + counts.back());
		    }
		    bounds.push_back(ibis::util::compactValue
				     (bounds.back(), DBL_MAX));
		}
		else { // too many values, reduce to 1000 bins
		    array_t<double> vals(hist.size());
		    array_t<uint32_t> cnts(hist.size());
		    vals.clear();
		    cnts.clear();
		    for (ibis::index::histogram::const_iterator hit =
			     hist.begin();
			 hit != hist.end(); ++ hit) {
			vals.push_back((*hit).first);
			cnts.push_back((*hit).second);
		    }
		    array_t<uint32_t> dvd(1000);
		    ibis::index::divideCounts(dvd, cnts);
		    bounds.push_back(vals[0]);
		    counts.push_back(0);
		    for (uint32_t i = 0; i < dvd.size(); ++ i) {
			uint32_t cnt = counts.back();
			for (uint32_t j = (i>0?dvd[i-1]:0); j < dvd[i]; ++ j)
			    cnt += cnts[j];
			counts.push_back(cnt);
			double bd;
			if (dvd[i] < vals.size())
			    bd = ibis::util::compactValue(vals[dvd[i]-1],
							  vals[dvd[i]]);
			else
			    bd = ibis::util::compactValue
				(vals.back(), DBL_MAX);
			bounds.push_back(bd);
		    }
		}
	    }
	    if (ierr >= 0)
		ierr = counts.size();
	    if (ierr > 0 && ibis::gVerbose > 2) {
		timer.stop();
		logMessage("getCumulativeDistribution",
			   "computing the distribution of "
			   "column %s with restriction \"%s\" took %g "
			   "sec(CPU) and %g sec(elapsed)", (*it).first,
			   constraints, timer.CPUTime(), timer.realTime());
	    }
	}
	if (ierr < 0)
	    ierr -= 10;
    }
    return ierr;
} // ibis::part::getCumulativeDistribution

/// Because most of the binning scheme leaves two bins for overflow, one
/// for values less than the expected minimum and one for values greater
/// than the expected maximum.  The minimum number of bins expected is
/// four (4).  This function will return error code -1 if the value of nbc
/// is less than 4.
long
ibis::part::getCumulativeDistribution
(const char *constraints, const char *name, uint32_t nbc,
 double *bounds, uint32_t *counts) const {
    if (nbc < 4) // work space too small
	return -1;

    std::vector<double> bds;
    std::vector<uint32_t> cts;
    long mbc = getCumulativeDistribution(constraints, name, bds, cts);
    mbc = packCumulativeDistribution(bds, cts, nbc, bounds, counts);
    return mbc;
} // ibis::part::getCumulativeDistribution

/// The arrays @c bounds1 and @c bounds2 are used if they contain values
/// in ascending order.  If they are empty or their values are not in
/// ascending order, then a simple linear binning will be used.  By
/// default, no more than 256 bins are used for each variable.
long
ibis::part::getJointDistribution(const char *constraints,
				 const char *name1, const char *name2,
				 std::vector<double>& bounds1,
				 std::vector<double>& bounds2,
				 std::vector<uint32_t>& counts) const {
    long ierr = -1;
    columnList::const_iterator it1 = columns.find(name1);
    columnList::const_iterator it2 = columns.find(name2);
    if (it1 == columns.end() || it2 == columns.end()) {
	if (it1 == columns.end())
	    logWarning("getJointDistribution", "%s is not known column name",
		       name1);
	if (it2 == columns.end())
	    logWarning("getJointDistribution", "%s is not known column name",
		       name2);
	return ierr;
    }

    const ibis::column *col1 = (*it1).second;
    const ibis::column *col2 = (*it2).second;
    ibis::horometer timer;
    if (ibis::gVerbose > 2)
	timer.start();
    ibis::bitvector mask;
    if (constraints != 0 || *constraints != 0) {
	ibis::query q(ibis::util::userName(), this);
	q.setWhereClause(constraints);
	ierr = q.evaluate();
	if (ierr < 0)
	    return ierr;
	const ibis::bitvector *hits = q.getHitVector();
	if (hits->cnt() == 0) // nothing to do any more
	    return 0;
	mask.copy(*hits);
    }
    else {
	col1->getNullMask(mask);
	ibis::bitvector tmp;
	col2->getNullMask(tmp);
	mask &= tmp;
    }

    counts.clear();
    switch (col1->type()) {
    case ibis::SHORT:
    case ibis::BYTE:
    case ibis::INT: {
	array_t<int32_t> *val1 = col1->selectInts(mask);
	if (val1 == 0) {
	    ierr = -4;
	    break;
	}
	array_t<int32_t> bnd1;
	if (bounds1.size() > 0) {
	    bnd1.resize(bounds1.size());
	    for (uint32_t i = 0; i < bounds1.size(); ++ i)
		bnd1[i] = static_cast<int32_t>(bounds1[i]);
	}
	switch (col2->type()) {
	case ibis::SHORT:
	case ibis::BYTE:
	case ibis::INT: {
	    array_t<int32_t> *val2 = col2->selectInts(mask);
	    if (val2 == 0) {
		ierr = -5;
		break;
	    }

	    array_t<int32_t> bnd2;
	    if (bounds2.size() > 0) {
		bnd2.resize(bounds2.size());
		for (uint32_t i = 0; i < bounds2.size(); ++ i)
		    bnd2[i] = static_cast<int32_t>(bounds2[i]);
	    }
	    ibis::index::mapValues(*val1, *val2, bnd1, bnd2, counts);
	    delete val2;
	    bounds1.resize(bnd1.size());
	    bounds2.resize(bnd2.size());
	    for (uint32_t i = 0; i < bnd1.size(); ++ i)
		bounds1[i] = bnd1[i];
	    for (uint32_t i = 0; i < bnd2.size(); ++ i)
		bounds2[i] = bnd2[i];
	    break;}
	case ibis::USHORT:
	case ibis::UBYTE:
	case ibis::UINT:
	case ibis::CATEGORY: {
	    array_t<uint32_t> *val2 = col2->selectUInts(mask);
	    if (val2 == 0) {
		ierr = -5;
		break;
	    }

	    array_t<uint32_t> bnd2;
	    if (bounds2.size() > 0) {
		bnd2.resize(bounds2.size());
		for (uint32_t i = 0; i < bounds2.size(); ++ i)
		    bnd2[i] = static_cast<uint32_t>(bounds2[i]);
	    }
	    ibis::index::mapValues(*val1, *val2, bnd1, bnd2, counts);
	    delete val2;
	    bounds1.resize(bnd1.size());
	    bounds2.resize(bnd2.size());
	    for (uint32_t i = 0; i < bnd1.size(); ++ i)
		bounds1[i] = bnd1[i];
	    for (uint32_t i = 0; i < bnd2.size(); ++ i)
		bounds2[i] = bnd2[i];
	    break;}
	case ibis::FLOAT: {
	    array_t<float> *val2 = col2->selectFloats(mask);
	    if (val2 == 0) {
		ierr = -5;
		break;
	    }

	    array_t<float> bnd2;
	    if (bounds2.size() > 0) {
		bnd2.resize(bounds2.size());
		for (uint32_t i = 0; i < bounds2.size(); ++ i)
		    bnd2[i] = static_cast<float>(bounds2[i]);
	    }
	    ibis::index::mapValues(*val1, *val2, bnd1, bnd2, counts);
	    delete val2;
	    bounds1.resize(bnd1.size());
	    bounds2.resize(bnd2.size());
	    for (uint32_t i = 0; i < bnd1.size(); ++ i)
		bounds1[i] = bnd1[i];
	    for (uint32_t i = 0; i < bnd2.size(); ++ i)
		bounds2[i] = bnd2[i];
	    break;}
	case ibis::DOUBLE: {
	    array_t<double> bnd2;
	    array_t<double> *val2 = col2->selectDoubles(mask);
	    if (val2 == 0) {
		ierr = -5;
		break;
	    }

	    if (bounds2.size() > 0) {
		bnd2.resize(bounds2.size());
		for (uint32_t i = 0; i < bounds2.size(); ++ i)
		    bnd2[i] = static_cast<double>(bounds2[i]);
	    }
	    ibis::index::mapValues(*val1, *val2, bnd1, bnd2, counts);
	    delete val2;
	    bounds1.resize(bnd1.size());
	    for (uint32_t i = 0; i < bnd1.size(); ++ i)
		bounds1[i] = bnd1[i];
	    bounds2.resize(bnd2.size());
	    for (uint32_t i = 0; i < bnd2.size(); ++ i)
		bounds2[i] = bnd2[i];
	    break;}
	default: {
	    ierr = -3;
	    logWarning("getJointDistribution",
		       "unable to handle column type %d",
		       static_cast<int>(col2->type()));
	    break;}
	}
	delete val1;
	break;}
    case ibis::USHORT:
    case ibis::UBYTE:
    case ibis::UINT:
    case ibis::CATEGORY: {
	array_t<uint32_t> *val1 = col1->selectUInts(mask);
	if (val1 == 0) {
	    ierr = -4;
	    break;
	}

	array_t<uint32_t> bnd1;
	if (bounds1.size() > 0) {
	    bnd1.resize(bounds1.size());
	    for (unsigned i = 0; i < bounds1.size(); ++ i)
		bnd1[i] = static_cast<uint32_t>(bounds1[i]);
	}
	switch (col2->type()) {
	case ibis::SHORT:
	case ibis::BYTE:
	case ibis::INT: {
	    array_t<int32_t> *val2 = col2->selectInts(mask);
	    if (val2 == 0) {
		ierr = -5;
		break;
	    }

	    array_t<int32_t> bnd2;
	    if (bounds2.size() > 0) {
		bnd2.resize(bounds2.size());
		for (uint32_t i = 0; i < bounds2.size(); ++ i)
		    bnd2[i] = static_cast<int32_t>(bounds2[i]);
	    }
	    ibis::index::mapValues(*val1, *val2, bnd1, bnd2, counts);
	    delete val2;
	    bounds1.resize(bnd1.size());
	    bounds2.resize(bnd2.size());
	    for (uint32_t i = 0; i < bnd1.size(); ++ i)
		bounds1[i] = bnd1[i];
	    for (uint32_t i = 0; i < bnd2.size(); ++ i)
		bounds2[i] = bnd2[i];
	    break;}
	case ibis::USHORT:
	case ibis::UBYTE:
	case ibis::UINT:
	case ibis::CATEGORY: {
	    array_t<uint32_t> *val2 = col2->selectUInts(mask);
	    if (val2 == 0) {
		ierr = -5;
		break;
	    }

	    array_t<uint32_t> bnd2;
	    if (bounds2.size() > 0) {
		bnd2.resize(bounds2.size());
		for (uint32_t i = 0; i < bounds2.size(); ++ i)
		    bnd2[i] = static_cast<uint32_t>(bounds2[i]);
	    }
	    ibis::index::mapValues(*val1, *val2, bnd1, bnd2, counts);
	    delete val2;
	    bounds1.resize(bnd1.size());
	    bounds2.resize(bnd2.size());
	    for (uint32_t i = 0; i < bnd1.size(); ++ i)
		bounds1[i] = bnd1[i];
	    for (uint32_t i = 0; i < bnd2.size(); ++ i)
		bounds2[i] = bnd2[i];
	    break;}
	case ibis::FLOAT: {
	    array_t<float> *val2 = col2->selectFloats(mask);
	    if (val2 == 0) {
		ierr = -5;
		break;
	    }

	    array_t<float> bnd2;
	    if (bounds2.size() > 0) {
		bnd2.resize(bounds2.size());
		for (uint32_t i = 0; i < bounds2.size(); ++ i)
		    bnd2[i] = static_cast<float>(bounds2[i]);
	    }
	    ibis::index::mapValues(*val1, *val2, bnd1, bnd2, counts);
	    delete val2;
	    bounds1.resize(bnd1.size());
	    bounds2.resize(bnd2.size());
	    for (uint32_t i = 0; i < bnd1.size(); ++ i)
		bounds1[i] = bnd1[i];
	    for (uint32_t i = 0; i < bnd2.size(); ++ i)
		bounds2[i] = bnd2[i];
	    break;}
	case ibis::DOUBLE: {
	    array_t<double> bnd2;
	    array_t<double> *val2 = col2->selectDoubles(mask);
	    if (val2 == 0) {
		ierr = -5;
		break;
	    }

	    if (bounds2.size() > 0) {
		bnd2.resize(bounds2.size());
		for (uint32_t i = 0; i < bounds2.size(); ++ i)
		    bnd2[i] = static_cast<double>(bounds2[i]);
	    }
	    ibis::index::mapValues(*val1, *val2, bnd1, bnd2, counts);
	    delete val2;
	    bounds1.resize(bnd1.size());
	    for (uint32_t i = 0; i < bnd1.size(); ++ i)
		bounds1[i] = bnd1[i];
	    bounds2.resize(bnd2.size());
	    for (uint32_t i = 0; i < bnd2.size(); ++ i)
		bounds2[i] = bnd2[i];
	    break;}
	default: {
	    ierr = -3;
	    logWarning("getJointDistribution",
		       "unable to handle column type %d",
		       static_cast<int>(col2->type()));
	    break;}
	}
	delete val1;
	break;}
    case ibis::FLOAT: {
	array_t<float> *val1 = col1->selectFloats(mask);
	if (val1 == 0) {
	    ierr = -4;
	    break;
	}

	array_t<float> bnd1;
	if (bounds1.size() > 0) {
	    bnd1.resize(bounds1.size());
	    for (uint32_t i = 0; i < bounds1.size(); ++ i)
		bnd1[i] = static_cast<float>(bounds1[i]);
	}
	switch (col2->type()) {
	case ibis::SHORT:
	case ibis::BYTE:
	case ibis::INT: {
	    array_t<int32_t> *val2 = col2->selectInts(mask);
	    if (val2 == 0) {
		ierr = -5;
		break;
	    }

	    array_t<int32_t> bnd2;
	    if (bounds2.size() > 0) {
		bnd2.resize(bounds2.size());
		for (uint32_t i = 0; i < bounds2.size(); ++ i)
		    bnd2[i] = static_cast<int32_t>(bounds2[i]);
	    }
	    ibis::index::mapValues(*val1, *val2, bnd1, bnd2, counts);
	    delete val2;
	    bounds1.resize(bnd1.size());
	    bounds2.resize(bnd2.size());
	    for (uint32_t i = 0; i < bnd1.size(); ++ i)
		bounds1[i] = bnd1[i];
	    for (uint32_t i = 0; i < bnd2.size(); ++ i)
		bounds2[i] = bnd2[i];
	    break;}
	case ibis::USHORT:
	case ibis::UBYTE:
	case ibis::UINT:
	case ibis::CATEGORY: {
	    array_t<uint32_t> *val2 = col2->selectUInts(mask);
	    if (val2 == 0) {
		ierr = -5;
		break;
	    }

	    array_t<uint32_t> bnd2;
	    if (bounds2.size() > 0) {
		bnd2.resize(bounds2.size());
		for (uint32_t i = 0; i < bounds2.size(); ++ i)
		    bnd2[i] = static_cast<uint32_t>(bounds2[i]);
	    }
	    ibis::index::mapValues(*val1, *val2, bnd1, bnd2, counts);
	    delete val2;
	    bounds1.resize(bnd1.size());
	    bounds2.resize(bnd2.size());
	    for (uint32_t i = 0; i < bnd1.size(); ++ i)
		bounds1[i] = bnd1[i];
	    for (uint32_t i = 0; i < bnd2.size(); ++ i)
		bounds2[i] = bnd2[i];
	    break;}
	case ibis::FLOAT: {
	    array_t<float> *val2 = col2->selectFloats(mask);
	    if (val2 == 0) {
		ierr = -5;
		break;
	    }

	    array_t<float> bnd2;
	    if (bounds2.size() > 0) {
		bnd2.resize(bounds2.size());
		for (uint32_t i = 0; i < bounds2.size(); ++ i)
		    bnd2[i] = static_cast<float>(bounds2[i]);
	    }
	    ibis::index::mapValues(*val1, *val2, bnd1, bnd2, counts);
	    delete val2;
	    bounds1.resize(bnd1.size());
	    bounds2.resize(bnd2.size());
	    for (uint32_t i = 0; i < bnd1.size(); ++ i)
		bounds1[i] = bnd1[i];
	    for (uint32_t i = 0; i < bnd2.size(); ++ i)
		bounds2[i] = bnd2[i];
	    break;}
	case ibis::DOUBLE: {
	    array_t<double> bnd2;
	    array_t<double> *val2 = col2->selectDoubles(mask);
	    if (val2 == 0) {
		ierr = -5;
		break;
	    }

	    if (bounds2.size() > 0) {
		bnd2.resize(bounds2.size());
		for (uint32_t i = 0; i < bounds2.size(); ++ i)
		    bnd2[i] = static_cast<double>(bounds2[i]);
	    }
	    ibis::index::mapValues(*val1, *val2, bnd1, bnd2, counts);
	    delete val2;
	    bounds1.resize(bnd1.size());
	    for (uint32_t i = 0; i < bnd1.size(); ++ i)
		bounds1[i] = bnd1[i];
	    bounds2.resize(bnd2.size());
	    for (uint32_t i = 0; i < bnd2.size(); ++ i)
		bounds2[i] = bnd2[i];
	    break;}
	default: {
	    ierr = -3;
	    logWarning("getJointDistribution",
		       "unable to handle column type %d",
		       static_cast<int>(col2->type()));
	    break;}
	}
	delete val1;
	break;}
    case ibis::DOUBLE: {
	array_t<double> *val1 = col1->selectDoubles(mask);
	if (val1 == 0) {
	    ierr = -4;
	    break;
	}

	array_t<double> bnd1;
	if (bounds1.size() > 0) {
	    bnd1.resize(bounds1.size());
	    for (uint32_t i = 0; i < bounds1.size(); ++ i)
		bnd1[i] = bounds1[i];
	}
	switch (col2->type()) {
	case ibis::SHORT:
	case ibis::BYTE:
	case ibis::INT: {
	    array_t<int32_t> *val2 = col2->selectInts(mask);
	    if (val2 == 0) {
		ierr = -5;
		break;
	    }

	    array_t<int32_t> bnd2;
	    if (bounds2.size() > 0) {
		bnd2.resize(bounds2.size());
		for (uint32_t i = 0; i < bounds2.size(); ++ i)
		    bnd2[i] = static_cast<int32_t>(bounds2[i]);
	    }
	    ibis::index::mapValues(*val1, *val2, bnd1, bnd2, counts);
	    delete val2;
	    bounds1.resize(bnd1.size());
	    bounds2.resize(bnd2.size());
	    for (uint32_t i = 0; i < bnd1.size(); ++ i)
		bounds1[i] = bnd1[i];
	    for (uint32_t i = 0; i < bnd2.size(); ++ i)
		bounds2[i] = bnd2[i];
	    break;}
	case ibis::USHORT:
	case ibis::UBYTE:
	case ibis::UINT:
	case ibis::CATEGORY: {
	    array_t<uint32_t> *val2 = col2->selectUInts(mask);
	    if (val2 == 0) {
		ierr = -5;
		break;
	    }

	    array_t<uint32_t> bnd2;
	    if (bounds2.size() > 0) {
		bnd2.resize(bounds2.size());
		for (uint32_t i = 0; i < bounds2.size(); ++ i)
		    bnd2[i] = static_cast<uint32_t>(bounds2[i]);
	    }
	    ibis::index::mapValues(*val1, *val2, bnd1, bnd2, counts);
	    delete val2;
	    bounds1.resize(bnd1.size());
	    bounds2.resize(bnd2.size());
	    for (uint32_t i = 0; i < bnd1.size(); ++ i)
		bounds1[i] = bnd1[i];
	    for (uint32_t i = 0; i < bnd2.size(); ++ i)
		bounds2[i] = bnd2[i];
	    break;}
	case ibis::FLOAT: {
	    array_t<float> *val2 = col2->selectFloats(mask);
	    if (val2 == 0) {
		ierr = -5;
		break;
	    }

	    array_t<float> bnd2;
	    if (bounds2.size() > 0) {
		bnd2.resize(bounds2.size());
		for (uint32_t i = 0; i < bounds2.size(); ++ i)
		    bnd2[i] = static_cast<float>(bounds2[i]);
	    }
	    ibis::index::mapValues(*val1, *val2, bnd1, bnd2, counts);
	    delete val2;
	    bounds1.resize(bnd1.size());
	    bounds2.resize(bnd2.size());
	    for (uint32_t i = 0; i < bnd1.size(); ++ i)
		bounds1[i] = bnd1[i];
	    for (uint32_t i = 0; i < bnd2.size(); ++ i)
		bounds2[i] = bnd2[i];
	    break;}
	case ibis::DOUBLE: {
	    array_t<double> bnd2;
	    array_t<double> *val2 = col2->selectDoubles(mask);
	    if (val2 == 0) {
		ierr = -5;
		break;
	    }

	    if (bounds2.size() > 0) {
		bnd2.resize(bounds2.size());
		for (uint32_t i = 0; i < bounds2.size(); ++ i)
		    bnd2[i] = static_cast<double>(bounds2[i]);
	    }
	    ibis::index::mapValues(*val1, *val2, bnd1, bnd2, counts);
	    delete val2;
	    bounds1.resize(bnd1.size());
	    for (uint32_t i = 0; i < bnd1.size(); ++ i)
		bounds1[i] = bnd1[i];
	    bounds2.resize(bnd2.size());
	    for (uint32_t i = 0; i < bnd2.size(); ++ i)
		bounds2[i] = bnd2[i];
	    break;}
	default: {
	    ierr = -3;
	    logWarning("getJointDistribution",
		       "unable to handle column type %d",
		       static_cast<int>(col2->type()));
	    break;}
	}
	delete val1;
	break;}
    default: {
	ierr = -3;
	logWarning("getJointDistribution",
		   "unable to handle column type %d",
		   static_cast<int>(col1->type()));
	break;}
    }

    if ((bounds1.size()+1) * (bounds2.size()+1) == counts.size())
	ierr = counts.size();
    else
	ierr = -2;
    if (ierr > 0 && ibis::gVerbose > 2) {
	timer.stop();
	logMessage("getJointDistribution",
		   "computing the joint distribution of "
		   "column %s and %s%s%s took %g "
		   "sec(CPU) and %g sec(elapsed)",
		   (*it1).first, (*it2).first,
		   (constraints ? " with restriction " : ""),
		   (constraints ? constraints : ""),
		   timer.CPUTime(), timer.realTime());
    }
    return ierr;
} // ibis::part::getJointDistribution

long ibis::part::packDistribution
(const std::vector<double>& bds, const std::vector<uint32_t>& cts,
 uint32_t nbc, double *bptr, uint32_t *cptr) const {
    uint32_t mbc = bds.size();
    if (mbc <= 0)
	return mbc;
    if (static_cast<uint32_t>(mbc+1) != cts.size()) {
	ibis::util::logMessage
	    ("Warning", "packDistribution expects the size "
	     "of bds[%lu] to be the one less than that of "
	     "cts[%lu], but it is not",
	     static_cast<long unsigned>(bds.size()),
	     static_cast<long unsigned>(cts.size()));
	return -1;
    }
    if (nbc < 2) {
	ibis::util::logMessage
	    ("Warning", "a binned distribution needs "
	     "two arrays of size at least 2, caller has "
	     "provided two arrays of size %lu",
	     static_cast<long unsigned>(nbc));
	return -2;
    }
    if (static_cast<uint32_t>(mbc) <= nbc) {
	// copy the values in bds and cts
	for (uint32_t i = 0; i < mbc; ++ i) {
	    bptr[i] = bds[i];
	    cptr[i] = cts[i];
	}
	cptr[mbc] = cts[mbc];
	++ mbc;
    }
    else { // make the distribution fit the given space
	// the first entry is always copied
	bptr[0] = bds[0];
	cptr[0] = cts[0];

	uint32_t top = 0; // the total number of entries to be redistributed
	uint32_t cnt = 0; // entries already redistributed
	for (uint32_t k = 1; k < mbc; ++ k)
	    top += cts[k];
	uint32_t i = 1; // index to output bins
	uint32_t j = 1; // index to input bins
	while (i < nbc-1 && nbc+j < mbc+i) {
	    uint32_t next = j + 1;
	    uint32_t tgt = (top - cnt) / (nbc-i-1);
	    bptr[i] = bds[j];
	    cptr[i] = cts[j];
	    while (cptr[i] < tgt && nbc+next <= mbc+i) {
		cptr[i] += cts[next];
		++ next;
	    }
#if defined(DEBUG) && DEBUG + 0 > 1
	    LOGGER(0)
		<< "DEBUG -- i=" << i << ", j = " << j << ", bds[j]="
		<< bds[j] << ", next=" << next << ", bds[next]="
		<< bds[next] << ", cts[next]=" << cts[next];
#endif
	    j = next;
	    ++ i;
	}
	++ j;
	if (mbc - j > nbc - i)
	    j = 1 + mbc - nbc + i;
	// copy the last few bins
	while (i < nbc && j < static_cast<uint32_t>(mbc)) {
	    bptr[i] = bds[j];
	    cptr[i] = cts[j];
	    ++ i;
	    ++ j;
	}
	if (j == static_cast<uint32_t>(mbc) && i < nbc) {
	    cptr[i] = cts[mbc];
	    mbc = i + 1;
	}
	else {
	    mbc = i;
	}
    }
    return mbc;
} // ibis::part::packDistribution

long ibis::part::packCumulativeDistribution
(const std::vector<double>& bds, const std::vector<uint32_t>& cts,
 uint32_t nbc, double *bptr, uint32_t *cptr) const {
    long mbc = bds.size();
    if (mbc <= 0)
	return mbc;
    if (static_cast<uint32_t>(mbc) != cts.size()) {
	ibis::util::logMessage
	    ("Warning", "packCumulativeDistribution expects "
	     "the size of bds[%lu] to be the same as that "
	     "of cts[%lu], but they are not",
	     static_cast<long unsigned>(bds.size()),
	     static_cast<long unsigned>(cts.size()));
	return -1;
    }
    if (nbc < 2) {
	ibis::util::logMessage
	    ("Warning", "a cumulative distribution needs "
	     "two arrays of size at least 2, caller has "
	     "provided two arrays of size %lu",
	     static_cast<long unsigned>(nbc));
	return -2;
    }
    if (static_cast<uint32_t>(mbc) <= nbc) {
	// copy the values in bds and cts
	for (int i = 0; i < mbc; ++ i) {
	    bptr[i] = bds[i];
	    cptr[i] = cts[i];
	}
    }
//     else if (static_cast<uint32_t>(mbc) <= nbc+nbc-3) {
// 	// less than two values in a bin on average
// 	uint32_t start = nbc + nbc - mbc - 2;
// 	for (uint32_t i = 0; i <= start; ++ i) {
// 	    bptr[i] = bds[i];
// 	    cptr[i] = cts[i];
// 	}
// 	for (uint32_t i = start+1; i < nbc-1; ++ i) {
// 	    uint32_t j = i + i - start;
// 	    bptr[i] = bds[j];
// 	    cptr[i] = cts[j];
// 	}
// 	bptr[nbc-1] = bds[mbc-1];
// 	cptr[nbc-1] = cts[mbc-1];
// 	mbc = nbc; // mbc is the return value
//     }
    else { // make the distribution fit the given space
	// the first entries are always copied
	bptr[0] = bds[0];
	cptr[0] = cts[0];
	bptr[1] = bds[1];
	cptr[1] = cts[1];

	uint32_t top = cts[mbc-2];
	uint32_t i = 2, j = 1;
	while (i < nbc-1 && nbc+j < mbc+i-1) {
	    uint32_t next = j + 1;
	    uint32_t tgt = cts[j] + (top - cts[j]) / (nbc-i-1);
	    while (cts[next] < tgt && nbc+next <= mbc+i-1)
		++ next;
#if defined(DEBUG) && DEBUG + 0 > 1
	    LOGGER(0)
		<< "DEBUG -- i=" << i << ", next=" << next << ", bds[next]="
		<< bds[next] << ", cts[next]=" << cts[next];
#endif
	    bptr[i] = bds[next];
	    cptr[i] = cts[next];
	    j = next;
	    ++ i;
	}
	++ j;
	if (mbc - j > nbc - i)
	    j = mbc - nbc + i;
	while (i < nbc && j < static_cast<uint32_t>(mbc)) {
	    bptr[i] = bds[j];
	    cptr[i] = cts[j];
	    ++ i;
	    ++ j;
	}
	mbc = i;
    }
    return mbc;
} // ibis::part::packCumulativeDistribution

// Construct an info object from a list of columns
ibis::part::info::info(const char* na, const char* de, const uint64_t& nr,
		       const ibis::part::columnList& co)
    : name(na), description(de), metaTags(0), nrows(nr) {
    columnList::const_iterator it;
    for (it = co.begin(); it != co.end(); ++ it)
	cols.push_back(new ibis::column::info(*((*it).second)));
}

// Construct an info object from a partitioin of a table
ibis::part::info::info(const part& tbl)
    : name(tbl.name()), description(tbl.description()),
      metaTags(tbl.metaTags().c_str()), nrows(tbl.nRows()) {
    columnList::const_iterator it;
    for (it = tbl.columns.begin(); it != tbl.columns.end(); ++ it)
	cols.push_back(new ibis::column::info(*((*it).second)));
}

// the destrubtor for the info class
ibis::part::info::~info() {
    for (std::vector< ibis::column::info* >::iterator it = cols.begin();
	 it != cols.end(); ++it)
	delete *it;
    cols.clear();
}

void ibis::part::barrel::getNullMask(ibis::bitvector& mask) const {
    if (_tbl == 0) return; // can not do anything

    for (uint32_t i = 0; i < size(); ++ i) {
	ibis::bitvector tmp(_tbl->getMask());
	if (cols.size()<=i || cols[i]==0) {
	    const ibis::column* col = _tbl->getColumn(name(i));
	    if (col)
		col->getNullMask(tmp);
	    else
		_tbl->logWarning("barrell::getNullMask",
				 "can not find a column named \"%s\"",
				 name(i));
	}
	else {
	    cols[i]->getNullMask(tmp);
	}
	if (tmp.size() == _tbl->nRows()) {
	    if (mask.size() == _tbl->nRows())
		mask &= tmp;
	    else
		mask.copy(tmp);
	}
    }
} // ibis::part::barrel::getNullMask

long ibis::part::barrel::open(const ibis::part *t) {
    long ierr = 0;
    position = 0;
    if (t == 0 && _tbl == 0) {
	ierr = -1;
	ibis::util::logMessage("ibis::part::barrel::open",
			       "An ibis::part must be speficied");
	return ierr;
    }
    if (_tbl == 0) _tbl = t;
    if (t == 0) t = _tbl;

    if (size() == 0) return ierr; // nothing to do

    stores.resize(size());
    fdes.resize(size());
    cols.resize(size());
    for (uint32_t i = 0; i < size(); ++ i) {
	stores[i] = 0;
	fdes[i] = -1;
	cols[i] = 0;
    }

    std::string dfn = t->currentDataDir();
    uint32_t dirlen = dfn.size();
    if (dfn[dirlen-1] != DIRSEP) {
	dfn += DIRSEP;
	++ dirlen;
    }

    for (uint32_t i = 0; i < size(); ++ i) {
	ibis::column *col = t->getColumn(name(i));
	if (col == 0) {
	    fdes.resize(i);
	    close();
	    ierr = -2;
	    t->logWarning("barrel::open",
			  "failed to find a column named \"%s\"",
			  name(i));
	    return ierr;
	}
	// use the name from col variable to ensure the case is correct
	dfn += col->name();
	// use getFile first
	ierr = ibis::fileManager::instance().
	    getFile(dfn.c_str(), &(stores[i]));
	if (ierr == 0) {
	    stores[i]->beginUse();
	}
	else { // getFile failed, open the name file
	    fdes[i] = UnixOpen(dfn.c_str(), OPEN_READONLY);
	    if (fdes[i] < 0) {
		t->logWarning("barrel::open",
			      "failed to open file \"%s\"", dfn.c_str());
		fdes.resize(i);
		close();
		ierr = -3;
		return ierr;
	    }
	}
	if (size() > 1)
	    dfn.erase(dirlen);
	cols[i] = col;
    }
    if (ibis::gVerbose > 5) {
	if (size() > 1) {
	    t->logMessage("barrel::open",
			  "successfully opened %lu files from %s",
			  static_cast<long unsigned>(size()), dfn.c_str());
	    if (ibis::gVerbose > 7) {
		ibis::util::logger lg;
		if (stores[0])
		    lg.buffer() << "stores[0]: "
			      << static_cast<void*>(stores[0]->begin());
		else
		    lg.buffer() << "file " << fdes[0];
		for (uint32_t i = 1 ; i < size(); ++ i) {
		    lg.buffer() << ", ";
		    if (stores[i])
			lg.buffer() << "stores[" << i << "]"
				  << static_cast<void*>(stores[i]->begin());
		    else
			lg.buffer() << "file " << fdes[i];
		}
	    }
	}
	else if (stores[0]) {
	    t->logMessage("barrel::open", "successfully read %s into memory "
			  "at 0x%.8x", dfn.c_str(),
			  static_cast<void*>(stores[0]->begin()));
	}
	else {
	    t->logMessage("barrel::open", "successfully opened %s with "
			  "descriptor %d", dfn.c_str(), fdes[0]);
	}
    }
    return ierr;
} // ibis::part::barrel::open

long ibis::part::barrel::close() {
    for (uint32_t i = 0; i < stores.size(); ++ i) {
	if (stores[i])
	    stores[i]->endUse();
    }
    for (uint32_t i = 0; i < fdes.size(); ++ i)
	if (fdes[i] >= 0)
	    UnixClose(fdes[i]);
    stores.clear();
    fdes.clear();
    cols.clear();
    return 0;
} // ibis::part::barrel::close

/// Read the variable values from the current record.  All values are
/// internally stored as doubles.
long ibis::part::barrel::read() {
    long ierr = 0;
    for (uint32_t i = 0; i < size(); ++ i) {
	switch (cols[i]->type()) {
	case ibis::CATEGORY:
	case ibis::UINT:
	case ibis::TEXT: { // unsigned integer
	    unsigned utmp;
	    if (stores[i]) {
		utmp = *(reinterpret_cast<unsigned*>
			 (stores[i]->begin() +
			  sizeof(utmp) * position));
	    }
	    else {
		ierr = (sizeof(utmp) !=
			UnixRead(fdes[i], &utmp, sizeof(utmp)));
	    }
	    value(i) = utmp;
	    break;}
	case ibis::INT: { // signed integer
	    int itmp;
	    if (stores[i]) {
		itmp = *(reinterpret_cast<int*>
			 (stores[i]->begin() +
			  sizeof(itmp) * position));
	    }
	    else {
		ierr = (sizeof(itmp) !=
			UnixRead(fdes[i], &itmp, sizeof(itmp)));
	    }
	    value(i) = itmp;
	    break;}
	case ibis::FLOAT: {
	    // 4-byte IEEE floating-point values
	    float ftmp;
	    if (stores[i]) {
		ftmp = *(reinterpret_cast<float*>
			 (stores[i]->begin() +
			  sizeof(ftmp) * position));
	    }
	    else {
		ierr = (sizeof(ftmp) !=
			UnixRead(fdes[i], &ftmp, sizeof(ftmp)));
	    }
	    value(i) = ftmp;
	    break;}
	case ibis::DOUBLE: {
	    // 8-byte IEEE floating-point values
	    double dtmp;
	    if (stores[i]) {
		dtmp = *(reinterpret_cast<double*>
			 (stores[i]->begin() +
			  sizeof(dtmp) * position));
	    }
	    else {
		ierr = (sizeof(dtmp) !=
			UnixRead(fdes[i], &dtmp, sizeof(dtmp)));
	    }
	    value(i) = dtmp;
	    break;}
	case ibis::OID:
	default: {
	    ++ ierr;
	    _tbl->logWarning("barrel::read", "unable to evaluate "
			     "attribute of type %s (name: %s)",
			     ibis::TYPESTRING[(int)cols[i]->type()],
			     cols[i]->name());
	    break;}
	}
    }
    ++ position;
    return ierr;
} // ibis::part::barrel::read

/// Seek to the position of the specified record for all variables.
long ibis::part::barrel::seek(uint32_t pos) {
    if (pos == position) return 0;
    if (pos >= cols[0]->partition()->nRows()) return -1;

    long ierr = 0;
    uint32_t i = 0;
    while (ierr == 0 && i < size()) {
	if (fdes[i] >= 0) {
	    ierr = UnixSeek(fdes[i], cols[i]->elementSize()*pos, SEEK_SET);
	    if (ierr != (int32_t)-1)
		ierr = 0;
	}
	++ i;
    }
    if (ierr < 0) { // rollback the file pointers
	while (i > 0) {
	    -- i;
	    if (fdes[i] >= 0)
		ierr = UnixSeek(fdes[i], cols[i]->elementSize()*position,
				SEEK_SET);
	}
    }
    else {
	position = pos;
    }
    return ierr;
} // ibis::part::barrel::seek

ibis::part::vault::vault(const ibis::roster& r)
    : barrel(r.getColumn()->partition()), _roster(r) {
    (void) recordVariable(r.getColumn()->name());
}

/// The function @c valut::open different from barrel::open in that it
/// opens the .srt file for the first variable.
long ibis::part::vault::open(const ibis::part *t) {
    long ierr = 0;
    position = 0;
    if (t == 0 && _tbl == 0) {
	ierr = -1;
	ibis::util::logMessage("ibis::part::vault::open",
			       "An ibis::part must be speficied");
	return ierr;
    }
    if (_tbl == 0) _tbl = t;
    if (t == 0) t = _tbl;

    if (size() == 0) return ierr; // nothing to do

    stores.resize(size());
    fdes.resize(size());
    cols.resize(size());
    for (uint32_t i = 0; i < size(); ++ i) {
	stores[i] = 0;
	fdes[i] = -1;
	cols[i] = 0;
    }

    std::string dfn = t->currentDataDir();
    uint32_t dirlen = dfn.size();
    if (dfn[dirlen-1] != DIRSEP) {
	dfn += DIRSEP;
	++ dirlen;
    }

    { // for variable 0, read the .srt file
	ibis::column *col = t->getColumn(name(0));
	if (col == 0) {
	    fdes.resize(0);
	    close();
	    ierr = -2;
	    t->logWarning("vault::open",
			  "failed to find a column named \"%s\"",
			  name(0));
	    return ierr;
	}

	dfn += col->name();
	dfn += ".srt"; // .srt file name
	// use getFile first
	ierr = ibis::fileManager::instance().
	    getFile(dfn.c_str(), &(stores[0]));
	if (ierr == 0) {
	    stores[0]->beginUse();
	}
	else { // getFile failed, open the name file
	    fdes[0] = UnixOpen(dfn.c_str(), OPEN_READONLY);
	    if (fdes[0] < 0) {
		t->logWarning("vault::open",
			      "failed to open file \"%s\"", dfn.c_str());
		fdes.resize(0);
		close();
		ierr = -3;
		return ierr;
	    }
	}
	if (ibis::gVerbose > 5) {
	    if (stores[0])
		t->logMessage("vault::open", "successfully read %s "
			      "(0x%.8x) for variable %s",
			      dfn.c_str(),
			      static_cast<void*>(stores[0]->begin()),
			      col->name());
	    else
		t->logMessage("vault::open",
			      "successfully opened %s with descriptor %d "
			      "for variable %s",
			      dfn.c_str(), fdes[0], col->name());
	}
	dfn.erase(dirlen);
	cols[0] = col;
    }

    // the remaining variables are opened the same way as in barrel
    for (uint32_t i = 1; i < size(); ++ i) {
	ibis::column *col = t->getColumn(name(i));
	if (col == 0) {
	    fdes.resize(i);
	    close();
	    ierr = -2;
	    t->logWarning("vault::open",
			  "failed to find a column named \"%s\"",
			  name(i));
	    return ierr;
	}

	dfn += col->name(); // the data file name
	// use getFile first
	ierr = ibis::fileManager::instance().
	    getFile(dfn.c_str(), &(stores[i]));
	if (ierr == 0) {
	    stores[i]->beginUse();
	}
	else { // getFile failed, open the name file
	    fdes[i] = UnixOpen(dfn.c_str(), OPEN_READONLY);
	    if (fdes[i] < 0) {
		t->logWarning("vault::open",
			      "failed to open file \"%s\"", dfn.c_str());
		fdes.resize(i);
		close();
		ierr = -3;
		return ierr;
	    }
	}
	dfn.erase(dirlen);
	cols[i] = col;
    }
    if (ibis::gVerbose > 5 && size() > 1) {
	t->logMessage("vault::open", "successfully opened %lu files from %s",
		      static_cast<long unsigned>(size()), dfn.c_str());
	if (ibis::gVerbose > 7) {
	    ibis::util::logger lg;
	    if (stores[0])
		lg.buffer() << "0x" << static_cast<void*>(stores[0]->begin());
	    else
		lg.buffer() << "file " << fdes[0];
	    for (uint32_t i = 1 ; i < size(); ++ i) {
		lg.buffer() << ", ";
		if (stores[i])
		    lg.buffer() << "0x"
			      << static_cast<void*>(stores[i]->begin());
		else
		    lg.buffer() << "file " << fdes[i];
	    }
	}
    }
    return ierr;
} // ibis::part::vault::open

/// Read the records indicated by @c position.  Treat @c position as the
/// logical position, the physical position is @c _roster[position].
long ibis::part::vault::read() {
    if (position >= _roster.size()) return -1; // out of bounds

    long ierr = 0;

    // the first variable is being read sequentially
    switch (cols[0]->type()) {
    case ibis::CATEGORY:
    case ibis::UINT:
    case ibis::TEXT: { // unsigned integer
	unsigned utmp;
	if (stores[0]) {
	    utmp = *(reinterpret_cast<unsigned*>
		     (stores[0]->begin() +
		      sizeof(utmp) * position));
	}
	else {
	    ierr = UnixSeek(fdes[0], sizeof(utmp)*position, SEEK_SET);
	    ierr = (sizeof(utmp) !=
		    UnixRead(fdes[0], &utmp, sizeof(utmp)));
	}
	value(0) = utmp;
	break;}
    case ibis::INT: { // signed integer
	int itmp;
	if (stores[0]) {
	    itmp = *(reinterpret_cast<int*>
		     (stores[0]->begin() +
		      sizeof(itmp) * position));
	}
	else {
	    ierr = UnixSeek(fdes[0], sizeof(itmp)*position, SEEK_SET);
	    ierr = (sizeof(itmp) !=
		    UnixRead(fdes[0], &itmp, sizeof(itmp)));
	}
	value(0) = itmp;
	break;}
    case ibis::FLOAT: {
	// 4-byte IEEE floating-point values
	float ftmp;
	if (stores[0]) {
	    ftmp = *(reinterpret_cast<float*>
		     (stores[0]->begin() +
		      sizeof(ftmp) * position));
	}
	else {
	    ierr = UnixSeek(fdes[0], sizeof(ftmp)*position, SEEK_SET);
	    ierr = (sizeof(ftmp) !=
		    UnixRead(fdes[0], &ftmp, sizeof(ftmp)));
	}
	value(0) = ftmp;
	break;}
    case ibis::DOUBLE: {
	// 8-byte IEEE floating-point values
	double dtmp;
	if (stores[0]) {
	    dtmp = *(reinterpret_cast<double*>
		     (stores[0]->begin() +
		      sizeof(dtmp) * position));
	}
	else {
	    ierr = UnixSeek(fdes[0], sizeof(dtmp)*position, SEEK_SET);
	    ierr = (sizeof(dtmp) !=
		    UnixRead(fdes[0], &dtmp, sizeof(dtmp)));
	}
	value(0) = dtmp;
	break;}
    case ibis::OID:
    default: {
	++ ierr;
	_tbl->logWarning("vault::read", "unable to evaluate "
			 "attribute of type %s (name: %s)",
			 ibis::TYPESTRING[(int)cols[0]->type()],
			 cols[0]->name());
	break;}
    }

    uint32_t pos = _roster[position]; // the actual position in files
    for (uint32_t i = 1; i < size(); ++ i) {
	switch (cols[i]->type()) {
	case ibis::CATEGORY:
	case ibis::UINT:
	case ibis::TEXT: { // unsigned integer
	    unsigned utmp;
	    if (stores[i]) {
		utmp = *(reinterpret_cast<unsigned*>
			 (stores[i]->begin() +
			  sizeof(utmp) * pos));
	    }
	    else {
		ierr = UnixSeek(fdes[i], sizeof(utmp)*pos, SEEK_SET);
		ierr = (sizeof(utmp) !=
			UnixRead(fdes[i], &utmp, sizeof(utmp)));
	    }
	    value(i) = utmp;
	    break;}
	case ibis::INT: { // signed integer
	    int itmp;
	    if (stores[i]) {
		itmp = *(reinterpret_cast<int*>
			 (stores[i]->begin() +
			  sizeof(itmp) * pos));
	    }
	    else {
		ierr = UnixSeek(fdes[i], sizeof(itmp)*pos, SEEK_SET);
		ierr = (sizeof(itmp) !=
			UnixRead(fdes[i], &itmp, sizeof(itmp)));
	    }
	    value(i) = itmp;
	    break;}
	case ibis::FLOAT: {
	    // 4-byte IEEE floating-point values
	    float ftmp;
	    if (stores[i]) {
		ftmp = *(reinterpret_cast<float*>
			 (stores[i]->begin() +
			  sizeof(ftmp) * pos));
	    }
	    else {
		ierr = UnixSeek(fdes[i], sizeof(ftmp)*pos, SEEK_SET);
		ierr = (sizeof(ftmp) !=
			UnixRead(fdes[i], &ftmp, sizeof(ftmp)));
	    }
	    value(i) = ftmp;
	    break;}
	case ibis::DOUBLE: {
	    // 8-byte IEEE floating-point values
	    double dtmp;
	    if (stores[i]) {
		dtmp = *(reinterpret_cast<double*>
			 (stores[i]->begin() +
			  sizeof(dtmp) * pos));
	    }
	    else {
		ierr = UnixSeek(fdes[i], sizeof(dtmp)*pos, SEEK_SET);
		ierr = (sizeof(dtmp) !=
			UnixRead(fdes[i], &dtmp, sizeof(dtmp)));
	    }
	    value(i) = dtmp;
	    break;}
	case ibis::OID:
	default: {
	    ++ ierr;
	    _tbl->logWarning("vault::read", "unable to evaluate "
			     "attribute of type %s (name: %s)",
			     ibis::TYPESTRING[(int)cols[i]->type()],
			     cols[i]->name());
	    break;}
	}
    }
    ++ position;
    return ierr;
} // ibis::part::vault::read

/// Change the logical position of the files.
long ibis::part::vault::seek(uint32_t pos) {
    long ierr = 0;
    if (pos != position) {
	if (pos < _roster.size()) { // good
	    if (fdes[0] >= 0)
		ierr = UnixSeek(fdes[0], cols[0]->elementSize()*pos,
				SEEK_SET);
	    if (ierr == 0)
		position = pos;
	}
	else {
	    ierr = -1;
	}
    }
    return ierr;
} // ibis::part::vault::seek

long ibis::part::vault::seek(double val) {
    long ierr = 0;

    if (stores[0] != 0) { // data in-memory
	switch (cols[0]->type()) {
	case ibis::CATEGORY:
	case ibis::UINT:
	case ibis::TEXT: { // unsigned integer
	    array_t<uint32_t> array(*stores[0]);
	    unsigned tgt = (val <= 0.0 ? 0 :
			    static_cast<uint32_t>(ceil(val)));
	    position = array.find(tgt);
	    break;}
	case ibis::INT: { // signed integer
	    array_t<int32_t> array(*stores[0]);
	    position = array.find(static_cast<int32_t>(ceil(val)));
	    break;}
	case ibis::FLOAT: {
	    // 4-byte IEEE floating-point values
	    array_t<float> array(*stores[0]);
	    position = array.find(static_cast<float>(val));
	    break;}
	case ibis::DOUBLE: {
	    // 8-byte IEEE floating-point values
	    array_t<double> array(*stores[0]);
	    position = array.find(val);
	    break;}
	case ibis::OID:
	default: {
	    ierr = -2;
	    _tbl->logWarning("vault::seek", "unable to evaluate "
			     "attribute of type %s (name: %s)",
			     ibis::TYPESTRING[(int)cols[0]->type()],
			     cols[0]->name());
	    break;}
	}	
    }
    else { // has to go through a file one value at a time
	switch (cols[0]->type()) {
	case ibis::CATEGORY:
	case ibis::UINT:
	case ibis::TEXT: { // unsigned integer
	    uint32_t tgt = (val <= 0.0 ? 0 :
			    static_cast<uint32_t>(ceil(val)));
	    position = seekValue<uint32_t>(fdes[0], tgt);
	    break;}
	case ibis::INT: { // signed integer
	    position = seekValue<int32_t>(fdes[0],
					  static_cast<int32_t>(ceil(val)));
	    break;}
	case ibis::FLOAT: {
	    // 4-byte IEEE floating-point values
	    position = seekValue<float>(fdes[0], static_cast<float>(val));
	    break;}
	case ibis::DOUBLE: {
	    // 8-byte IEEE floating-point values
	    position = seekValue<double>(fdes[0], val);
	    break;}
	case ibis::OID:
	default: {
	    ierr = -2;
	    _tbl->logWarning("vault::seek", "unable to evaluate "
			     "attribute of type %s (name: %s)",
			     ibis::TYPESTRING[(int)cols[0]->type()],
			     cols[0]->name());
	    break;}
	}
    }
    return ierr;
} // ibis::part::vault::seek

/// Can not fit the value into memory, has to read one value from the file
/// one at a time to do the comparison
template <class T>
uint32_t ibis::part::vault::seekValue(int fd, const T& val) const {
    long ierr;
    uint32_t i = 0;
    uint32_t j = _roster.size();
    uint32_t m = (i + j) / 2;
    while (i < m) {
	T tmp;
	uint32_t pos = sizeof(T) * _roster[m];
	ierr = UnixSeek(fd, pos, SEEK_SET);
	if (ierr < 0)
	    return _roster.size();
	ierr = UnixRead(fd, &tmp, sizeof(T));
	if (ierr < 0)
	    return _roster.size();
	if (tmp < val)
	    i = m;
	else
	    j = m;
	m = (i + j) / 2;
    }
    if (i == 0) { // it is possible that 0th value has not been checked
	T tmp;
	uint32_t pos = sizeof(T) * _roster[0];
	ierr = UnixSeek(fd, pos, SEEK_SET);
	if (ierr < 0)
	    return _roster.size();
	ierr = UnixRead(fd, &tmp, sizeof(T));
	if (ierr < 0)
	    return _roster.size();
	if (tmp >= val)
	    j = 0;
    }
    return j;
} // ibis::part::vault::seekValue

template <class T>
uint32_t ibis::part::vault::seekValue(const array_t<T>&arr,
				      const T& val) const {
    uint32_t i = 0;
    uint32_t j = _roster.size();
    uint32_t m = (i + j) / 2;
    while (i < m) {
	uint32_t pos = _roster[m];
	if (arr[pos] < val)
	    i = m;
	else
	    j = m;
	m = (i + j) / 2;
    }
    if (i == 0) { // it is possible that 0th value has not been checked
	uint32_t pos = _roster[m];
	if (arr[pos] >= val)
	    j = 0;
    }
    return j;
} // ibis::part::vault::seekValue

// This is not inlined because inlining it would make it necessary for
// part.h to include iroster.h.  Users of this function may directly call
// _roster[position] to avoid the function call overhead.
uint32_t ibis::part::vault::tellReal() const {
    return _roster[position];
} // ibis::part::vault::tellReal

/// Examining the given director to look for the metadata files and
/// constructs ibis::part.
void ibis::util::tablesFromDir(ibis::partList& tlist, const char *dir1) {
    if (dir1 == 0) return;
    try {
	if (ibis::gVerbose > 1)
	    logMessage("tablesFromDir", "examining %s", dir1);
	ibis::part* tmp = new ibis::part(dir1, 0);
	if (tmp) {
	    if (tmp->name() && tmp->nRows()) {
		ibis::util::mutexLock
		    lock(&ibis::util::envLock, "tablesFromDir");
		ibis::partList::iterator it = tlist.find(tmp->name());
		if (it != tlist.end()) { // deallocate the old table
		    logMessage("tablesFromDir", "replacing the old partition "
			       "named %s with new data from %s",
			       (*it).first, dir1);
		    delete (*it).second;
		    tlist.erase(it);
		}
		tlist[tmp->name()] = tmp;
	    }
	    else {
		if (ibis::gVerbose > 4)
		    logMessage("tablesFromDir", "directory %s "
			       "does not contain a valid \"-part.txt\" file "
			       "or contains an empty partition",
			       dir1);
		delete tmp;
	    }
	}
    }
    catch (const std::exception& e) {
	logMessage("tablesFromDir", "received a std::exception -- %s",
		   e.what());
    }
    catch (const char* s) {
	logMessage("tablesFromDir", "received a string exception -- %s",
		   s);
    }
    catch (...) {
	logMessage("tablesFromDir", "received an unexpected exception");
    }
#if defined(HAVE_DIRENT_H)
    // on unix machines, we know how to traverse the subdirectories
    // traverse the subdirectories to generate more tables
    char nm1[PATH_MAX];
    long len = strlen(dir1);

    DIR* dirp = opendir(dir1);
    if (dirp == 0) return;
    struct dirent* ent = 0;
    while ((ent = readdir(dirp)) != 0) {
	if ((ent->d_name[1] == 0 || ent->d_name[1] == '.') &&
	    ent->d_name[0] == '.') { // skip '.' and '..'
	    continue;
	}
	if (len + strlen(ent->d_name)+2 >= PATH_MAX) {
	    ibis::util::logMessage("tablesFromDir",
				   "name (%s%c%s) too long",
				   dir1, DIRSEP, ent->d_name);
	    continue;
	}

	sprintf(nm1, "%s%c%s", dir1, DIRSEP, ent->d_name);
	Stat_T st1;
	if (UnixStat(nm1, &st1)==0) {
	    if ((st1.st_mode & S_IFDIR) == S_IFDIR) {
		tablesFromDir(tlist, nm1);
	    }
	}
    }
    closedir(dirp);
#endif
} // ibis::util::tablesFromDir

// read the activeDir and backupDir -- if there are matching subdirs,
// construct an ibis::part from them
void ibis::util::tablesFromDir(ibis::partList &tables,
			       const char* adir, const char* bdir) {
    if (adir == 0 || *adir == 0) return;
    if (ibis::gVerbose > 1)
	logMessage("tablesFromDir", "examining %s (%s)", adir,
		   (bdir ? bdir : "?"));
    try {
	part* tbl = new ibis::part(adir, bdir);
	if (tbl->name() && tbl->nRows()>0 && tbl->nColumns()>0) {
	    ibis::util::mutexLock
		lock(&ibis::util::envLock, "tablesFromDir");
	    ibis::partList::const_iterator it =
		tables.find(tbl->name());
	    if (it == tables.end()) { // a new name
		tables[tbl->name()] = tbl;
		if (ibis::gVerbose > 1)
		    logMessage("tablesFromDir",
			       "add new partition \"%s\"", tbl->name());
	    }
	    else {
		if (ibis::gVerbose > 1)
		    logMessage("tablesFromDir", "there "
			       "is already an ibis::part with name of "
			       "\"%s\"(%s) in the list of tables "
			       "-- will discard the current "
			       "data partition from %s", tbl->name(),
			       (*it).second->currentDataDir(),
			       tbl->currentDataDir());
		delete tbl;
	    }
	}
	else {
	    if (ibis::gVerbose > 4)
		if (bdir == 0 || *bdir == 0)
		    logMessage("tablesFromDir", "directory "
			       "%s contains an invalid -part.txt or "
			       "an empty partition", adir);
		else
		    logMessage("tablesFromDir", "directories %s and %s "
			       "contain mismatching information or both of "
			       "them are empty", adir, bdir);
	    delete tbl;
	    tbl = 0;
	}
	if (tbl) return; // if a new part.has been created, return now
    }
    catch (const char* s) {
	logMessage("tablesFromDir", "received a string "
		   "exception -- %s", s);
    }
    catch (const std::exception& e) {
	logMessage("tablesFromDir", "received a library "
		   "exception -- %s", e.what());
    }
    catch (...) {
	logMessage("tablesFromDir", "received an unexpected "
		   "exception");
    }
#if defined(HAVE_DIRENT_H)
    if (bdir == 0) return; // must have both adir and bdir
    // on unix machines, the directories adir and bdir may contain
    // subdirectories -- this section of code reads the subdirectories to
    // generate more tables
    char nm1[PATH_MAX], nm2[PATH_MAX];
    uint32_t j = strlen(adir);
    uint32_t len = strlen(bdir);
    len = (len<j) ? j : len;

    DIR* dirp = opendir(adir);
    if (dirp == 0) return;
    struct dirent* ent = 0;
    while ((ent = readdir(dirp)) != 0) {
	if ((ent->d_name[1] == 0 || ent->d_name[1] == '.') &&
	    ent->d_name[0] == '.') { // skip '.' and '..'
	    continue;
	}
	if (len + strlen(ent->d_name)+2 >= PATH_MAX) {
	    logMessage("tablesFromDir",
		       "name (%s%c%s | %s%c%s) too long",
		       adir, DIRSEP, ent->d_name, bdir, DIRSEP, ent->d_name);
	    continue;
	}

	sprintf(nm1, "%s%c%s", adir, DIRSEP, ent->d_name);
	sprintf(nm2, "%s%c%s", bdir, DIRSEP, ent->d_name);
	struct stat st1, st2;
	if (stat(nm1, &st1)==0 && stat(nm2, &st2)==0) {
	    if (((st1.st_mode & S_IFDIR) == S_IFDIR) &&
		((st2.st_mode & S_IFDIR) == S_IFDIR)) {
		tablesFromDir(tables, nm1, nm2);
	    }
	}
    }
    closedir(dirp);
#endif
} // ibis::util::tablesFromDir

// read the parameters dataDir1 and dataDir2 to build tables
void ibis::util::tablesFromResources(ibis::partList& tables,
				     const ibis::resource &res) {
    const char* dir1 = res.getValue("activeDir");
    if (dir1 == 0) {
	dir1 = res.getValue("dataDir1");
	if (dir1 == 0) {
	    dir1 = res.getValue("activeDirectory");
	    if (dir1 == 0) {
		dir1 = res.getValue("dataDir");
		if (dir1 == 0) {
		    dir1 = res.getValue("dataDirectory");
		    if (dir1 == 0) {
			dir1 = res.getValue("indexDir");
			if (dir1 == 0) {
			    dir1 = res.getValue("indexDirectory");
			}
		    }
		}
	    }
	}
    }
    if (dir1) {
	const char* dir2 = res.getValue("backupDir");
	if (dir2 == 0) {
	    dir2 = res.getValue("DataDir2");
	    if (dir2 == 0) {
		dir2 = res.getValue("backupDirectory");
	    }
	}
	if (ibis::gVerbose > 1)
	    logMessage("tablesFromResources", "examining %s (%s)", dir1,
		       (dir2 ? dir2 : "?"));
	if (dir2 != 0 && *dir2 != 0)
	    tablesFromDir(tables, dir1, dir2);
	else
	    tablesFromDir(tables, dir1);
    }

    for (ibis::resource::gList::const_iterator it = res.gBegin();
	 it != res.gEnd(); ++it)
	tablesFromResources(tables, *((*it).second));
} // ibis::util::tablesFromResources

// explicit instantiations of the templated functions
template long
ibis::part::doScan(const array_t<char>&,
		   const ibis::qRange&, const ibis::bitvector&,
		   ibis::bitvector&) const;
template long
ibis::part::doScan(const array_t<signed char>&,
		   const ibis::qRange&, const ibis::bitvector&,
		   ibis::bitvector&) const;
template long
ibis::part::doScan(const array_t<unsigned char>&,
		   const ibis::qRange&, const ibis::bitvector&,
		   ibis::bitvector&) const;
template long
ibis::part::doScan(const array_t<int16_t>&,
		   const ibis::qRange&, const ibis::bitvector&,
		   ibis::bitvector&) const;
template long
ibis::part::doScan(const array_t<uint16_t>&,
		   const ibis::qRange&, const ibis::bitvector&,
		   ibis::bitvector&) const;
template long
ibis::part::doScan(const array_t<int32_t>&,
		   const ibis::qRange&, const ibis::bitvector&,
		   ibis::bitvector&) const;
template long
ibis::part::doScan(const array_t<uint32_t>&,
		   const ibis::qRange&, const ibis::bitvector&,
		   ibis::bitvector&) const;
template long
ibis::part::doScan(const array_t<int64_t>&,
		   const ibis::qRange&, const ibis::bitvector&,
		   ibis::bitvector&) const;
template long
ibis::part::doScan(const array_t<uint64_t>&,
		   const ibis::qRange&, const ibis::bitvector&,
		   ibis::bitvector&) const;
template long
ibis::part::doScan(const array_t<float>&,
		   const ibis::qRange&, const ibis::bitvector&,
		   ibis::bitvector&) const;
template long
ibis::part::doScan(const array_t<double>&,
		   const ibis::qRange&, const ibis::bitvector&,
		   ibis::bitvector&) const;
template long
ibis::part::doCompare(const array_t<char>&, const ibis::bitvector&,
		      ibis::bitvector&, const ibis::qRange&) const;
template long
ibis::part::doCompare<char>(const char*, const ibis::bitvector&,
			    ibis::bitvector&, const ibis::qRange&) const;
