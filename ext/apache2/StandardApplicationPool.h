#ifndef _PASSENGER_STANDARD_APPLICATION_POOL_H_
#define _PASSENGER_STANDARD_APPLICATION_POOL_H_

#include <boost/shared_ptr.hpp>
#include <boost/weak_ptr.hpp>
#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition.hpp>
#include <boost/bind.hpp>

#include <string>
#include <map>
#include <list>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctime>
#include <cerrno>
#ifdef TESTING_APPLICATION_POOL
	#include <cstdlib>
#endif

#include "ApplicationPool.h"
#ifdef PASSENGER_USE_DUMMY_SPAWN_MANAGER
	#include "DummySpawnManager.h"
#else
	#include "SpawnManager.h"
#endif

namespace Passenger {

using namespace std;
using namespace boost;

class ApplicationPoolServer;

/****************************************************************
 *
 *  See "doc/ApplicationPool algorithm.txt" for a more readable
 *  and detailed description of the algorithm implemented here.
 *
 ****************************************************************/

/**
 * A standard implementation of ApplicationPool for single-process environments.
 *
 * The environment may or may not be multithreaded - StandardApplicationPool is completely
 * thread-safe. Apache with the threading MPM is an example of a multithreaded single-process
 * environment.
 *
 * This class is unusable in multi-process environments such as Apache with the prefork MPM.
 * The reasons are as follows:
 *  - StandardApplicationPool uses threads internally. Because threads disappear after a fork(),
 *    a StandardApplicationPool object will become unusable after a fork().
 *  - StandardApplicationPool stores its internal cache on the heap. Different processes
 *    cannot share their heaps, so they will not be able to access each others' pool cache.
 *  - StandardApplicationPool has a connection to the spawn server. If there are multiple
 *    processes, and they all use the spawn servers's connection at the same time without
 *    some sort of synchronization, then bad things will happen.
 *
 * (Of course, StandardApplicationPool <em>is</em> usable if each process creates its own
 * StandardApplicationPool object, but that would defeat the point of having a shared pool.)
 *
 * For multi-process environments, one should use ApplicationPoolServer instead.
 *
 * @ingroup Support
 */
class StandardApplicationPool: public ApplicationPool {
private:
	static const int DEFAULT_MAX_IDLE_TIME = 120;
	static const int DEFAULT_MAX_POOL_SIZE = 20;

	friend class ApplicationPoolServer;
	struct AppContainer;
	
	typedef shared_ptr<AppContainer> AppContainerPtr;
	typedef list<AppContainerPtr> AppContainerList;
	typedef shared_ptr<AppContainerList> AppContainerListPtr;
	typedef map<string, AppContainerListPtr> ApplicationMap;
	
	struct AppContainer {
		ApplicationPtr app;
		time_t lastUsed;
		unsigned int sessions;
		AppContainerList::iterator iterator;
		AppContainerList::iterator ia_iterator;
	};
	
	struct SharedData {
		mutex lock;
		condition activeOrMaxChanged;
		
		ApplicationMap apps;
		unsigned int max;
		unsigned int count;
		unsigned int active;
		AppContainerList inactiveApps;
		map<string, time_t> restartFileTimes;
	};
	
	typedef shared_ptr<SharedData> SharedDataPtr;
	
	struct SessionCloseCallback {
		SharedDataPtr data;
		weak_ptr<AppContainer> container;
		
		SessionCloseCallback(SharedDataPtr data,
		                     const weak_ptr<AppContainer> &container) {
			this->data = data;
			this->container = container;
		}
		
		void operator()() {
			mutex::scoped_lock l(data->lock);
			AppContainerPtr container(this->container.lock());
			
			if (container == NULL) {
				return;
			}
			
			ApplicationMap::iterator it;
			it = data->apps.find(container->app->getAppRoot());
			if (it != data->apps.end()) {
				AppContainerListPtr list(it->second);
				container->lastUsed = time(NULL);
				container->sessions--;
				if (container->sessions == 0) {
					list->erase(container->iterator);
					list->push_front(container);
					container->iterator = list->begin();
					data->inactiveApps.push_back(container);
					container->ia_iterator = data->inactiveApps.end();
					container->ia_iterator--;
				}
				data->active--;
				data->activeOrMaxChanged.notify_all();
			}
		}
	};

	#ifdef PASSENGER_USE_DUMMY_SPAWN_MANAGER
		DummySpawnManager spawnManager;
	#else
		SpawnManager spawnManager;
	#endif
	SharedDataPtr data;
	thread *cleanerThread;
	bool detached;
	bool done;
	unsigned int maxIdleTime;
	condition cleanerThreadSleeper;
	
	// Shortcuts for instance variables in SharedData. Saves typing in get().
	mutex &lock;
	condition &activeOrMaxChanged;
	ApplicationMap &apps;
	unsigned int &max;
	unsigned int &count;
	unsigned int &active;
	AppContainerList &inactiveApps;
	map<string, time_t> &restartFileTimes;
	
	bool needsRestart(const string &appRoot) {
		string restartFile(appRoot);
		restartFile.append("/tmp/restart.txt");
		
		struct stat buf;
		bool result;
		if (stat(restartFile.c_str(), &buf) == 0) {
			int ret;
			#ifdef TESTING_APPLICATION_POOL
				if (getenv("nextRestartTxtDeletionShouldFail") != NULL) {
					unsetenv("nextRestartTxtDeletionShouldFail");
					ret = -1;
					errno = EACCES;
				} else {
					ret = unlink(restartFile.c_str());
				}
			#else
				do {
					ret = unlink(restartFile.c_str());
				} while (ret == -1 && errno == EAGAIN);
			#endif
			if (ret == 0 || errno == ENOENT) {
				restartFileTimes.erase(appRoot);
				result = true;
			} else {
				map<string, time_t>::const_iterator it;
				
				it = restartFileTimes.find(appRoot);
				if (it == restartFileTimes.end()) {
					result = true;
				} else {
					result = buf.st_mtime != restartFileTimes[appRoot];
				}
				restartFileTimes[appRoot] = buf.st_mtime;
			}
		} else {
			restartFileTimes.erase(appRoot);
			result = false;
		}
		return result;
	}
	
	void cleanerThreadMainLoop() {
		mutex::scoped_lock l(lock);
		while (!done) {
			xtime xt;
			xtime_get(&xt, TIME_UTC);
			xt.sec += maxIdleTime + 1;
			if (cleanerThreadSleeper.timed_wait(l, xt)) {
				// Condition was woken up.
				if (done) {
					// StandardApplicationPool is being destroyed.
					break;
				} else {
					continue;
				}
			}
			
			time_t now = time(NULL);
			AppContainerList::iterator it;
			for (it = inactiveApps.begin(); it != inactiveApps.end(); it++) {
				AppContainer &container(*it->get());
				ApplicationPtr app(container.app);
				AppContainerListPtr appList(apps[app->getAppRoot()]);
				
				if (now - container.lastUsed > (time_t) maxIdleTime) {
					P_TRACE("Cleaning idle app " << app->getAppRoot() <<
						" (PID " << app->getPid() << ")");
					appList->erase(container.iterator);
					
					AppContainerList::iterator prev = it;
					prev--;
					inactiveApps.erase(it);
					it = prev;
					
					count--;
				}
				if (appList->empty()) {
					apps.erase(app->getAppRoot());
					data->restartFileTimes.erase(app->getAppRoot());
				}
			}
		}
	}
	
	void detach() {
		detached = true;
		
		ApplicationMap::iterator it;
		for (it = apps.begin(); it != apps.end(); it++) {
			AppContainerList &list = *(it->second.get());
			AppContainerList::iterator it2;
			for (it2 = list.begin(); it2 != list.end(); it2++) {
				(*it2)->app->detach();
			}
		}
	}
	
	pair<AppContainerPtr, AppContainerList *>
	spawnOrUseExisting(mutex::scoped_lock &l, const string &appRoot,
	                   bool lowerPrivilege, const string &lowestUser) {
		AppContainerPtr container;
		AppContainerList *list;
		
		try {
			ApplicationMap::iterator it(apps.find(appRoot));
			
			if (it != apps.end() && needsRestart(appRoot)) {
				AppContainerList::iterator it2;
				list = it->second.get();
				for (it2 = list->begin(); it2 != list->end(); it2++) {
					container = *it2;
					if (container->sessions == 0) {
						inactiveApps.erase(container->ia_iterator);
					} else {
						active--;
					}
					it2--;
					list->erase(container->iterator);
					count--;
				}
				apps.erase(appRoot);
				spawnManager.reload(appRoot);
				it = apps.end();
			}
			
			if (it != apps.end()) {
				list = it->second.get();
		
				if (list->front()->sessions == 0 || count >= max) {
					container = list->front();
					list->pop_front();
					list->push_back(container);
					container->iterator = list->end();
					container->iterator--;
					if (container->sessions == 0) {
						inactiveApps.erase(container->ia_iterator);
					}
					active++;
				} else {
					container = ptr(new AppContainer());
					container->app = spawnManager.spawn(appRoot,
						lowerPrivilege, lowestUser);
					container->sessions = 0;
					list->push_back(container);
					container->iterator = list->end();
					container->iterator--;
					count++;
					active++;
					activeOrMaxChanged.notify_all();
				}
			} else {
				while (active >= max) {
					activeOrMaxChanged.wait(l);
				}
				if (count == max) {
					container = inactiveApps.front();
					inactiveApps.pop_front();
					list = apps[container->app->getAppRoot()].get();
					list->erase(container->iterator);
					if (list->empty()) {
						apps.erase(container->app->getAppRoot());
						restartFileTimes.erase(container->app->getAppRoot());
					}
					count--;
				}
				container = ptr(new AppContainer());
				container->app = spawnManager.spawn(appRoot, lowerPrivilege, lowestUser);
				container->sessions = 0;
				it = apps.find(appRoot);
				if (it == apps.end()) {
					list = new AppContainerList();
					apps[appRoot] = ptr(list);
				} else {
					list = it->second.get();
				}
				list->push_back(container);
				container->iterator = list->end();
				container->iterator--;
				count++;
				active++;
				activeOrMaxChanged.notify_all();
			}
		} catch (const SpawnException &e) {
			string message("Cannot spawn application '");
			message.append(appRoot);
			message.append("': ");
			message.append(e.what());
			if (e.hasErrorPage()) {
				throw SpawnException(message, e.getErrorPage());
			} else {
				throw SpawnException(message);
			}
		} catch (const exception &e) {
			string message("Cannot spawn application '");
			message.append(appRoot);
			message.append("': ");
			message.append(e.what());
			throw SpawnException(message);
		}
		
		return make_pair(container, list);
	}
	
public:
	/**
	 * Create a new StandardApplicationPool object.
	 *
	 * @param spawnServerCommand The filename of the spawn server to use.
	 * @param logFile Specify a log file that the spawn server should use.
	 *            Messages on its standard output and standard error channels
	 *            will be written to this log file. If an empty string is
	 *            specified, no log file will be used, and the spawn server
	 *            will use the same standard output/error channels as the
	 *            current process.
	 * @param environment The RAILS_ENV environment that all RoR applications
	 *            should use. If an empty string is specified, the current value
	 *            of the RAILS_ENV environment variable will be used.
	 * @param rubyCommand The Ruby interpreter's command.
	 * @throws SystemException An error occured while trying to setup the spawn server.
	 * @throws IOException The specified log file could not be opened.
	 */
	StandardApplicationPool(const string &spawnServerCommand,
	             const string &logFile = "",
	             const string &environment = "production",
	             const string &rubyCommand = "ruby",
	             const string &user = "")
	        :
		#ifndef PASSENGER_USE_DUMMY_SPAWN_MANAGER
		spawnManager(spawnServerCommand, logFile, environment, rubyCommand),
		#endif
		data(new SharedData()),
		lock(data->lock),
		activeOrMaxChanged(data->activeOrMaxChanged),
		apps(data->apps),
		max(data->max),
		count(data->count),
		active(data->active),
		inactiveApps(data->inactiveApps),
		restartFileTimes(data->restartFileTimes)
	{
		detached = false;
		done = false;
		max = DEFAULT_MAX_POOL_SIZE;
		count = 0;
		active = 0;
		maxIdleTime = DEFAULT_MAX_IDLE_TIME;
		cleanerThread = new thread(bind(&StandardApplicationPool::cleanerThreadMainLoop, this));
	}
	
	virtual ~StandardApplicationPool() {
		if (!detached) {
			{
				mutex::scoped_lock l(lock);
				done = true;
				cleanerThreadSleeper.notify_one();
			}
			cleanerThread->join();
		}
		delete cleanerThread;
	}
	
	virtual Application::SessionPtr
	get(const string &appRoot, bool lowerPrivilege = true, const string &lowestUser = "nobody") {
		unsigned int attempt;
		const unsigned int MAX_ATTEMPTS = 5;
		
		attempt = 0;
		while (true) {
			attempt++;
			
			mutex::scoped_lock l(lock);
			pair<AppContainerPtr, AppContainerList *> p(
				spawnOrUseExisting(l, appRoot, lowerPrivilege, lowestUser)
			);
			AppContainerPtr &container(p.first);
			AppContainerList &list(*p.second);
			
			container->lastUsed = time(NULL);
			container->sessions++;
			try {
				return container->app->connect(SessionCloseCallback(data, container));
			} catch (const exception &e) {
				container->sessions--;
				if (attempt == MAX_ATTEMPTS) {
					string message("Cannot connect to an existing application instance for '");
					message.append(appRoot);
					message.append("': ");
					try {
						const SystemException &syse = dynamic_cast<const SystemException &>(e);
						message.append(syse.sys());
					} catch (const bad_cast &) {
						message.append(e.what());
					}
					throw IOException(message);
				} else {
					list.erase(container->iterator);
					if (list.empty()) {
						apps.erase(appRoot);
					}
					count--;
					active--;
				}
			}
		}
		// Never reached; shut up compiler warning
		return Application::SessionPtr();
	}
	
	virtual void clear() {
		mutex::scoped_lock l(lock);
		apps.clear();
		inactiveApps.clear();
		restartFileTimes.clear();
		count = 0;
		active = 0;
	}
	
	virtual void setMaxIdleTime(unsigned int seconds) {
		mutex::scoped_lock l(lock);
		maxIdleTime = seconds;
		cleanerThreadSleeper.notify_one();
	}
	
	virtual void setMax(unsigned int max) {
		mutex::scoped_lock l(lock);
		this->max = max;
		activeOrMaxChanged.notify_all();
	}
	
	virtual unsigned int getActive() const {
		return active;
	}
	
	virtual unsigned int getCount() const {
		return count;
	}
	
	virtual pid_t getSpawnServerPid() const {
		return spawnManager.getServerPid();
	}
};

} // namespace Passenger

#endif /* _PASSENGER_STANDARD_APPLICATION_POOL_H_ */

