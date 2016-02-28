/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2016 Phusion Holding B.V.
 *
 *  "Passenger", "Phusion Passenger" and "Union Station" are registered
 *  trademarks of Phusion Holding B.V.
 *
 *  See LICENSE file for license information.
 */
#ifndef _PASSENGER_APPLICATION_POOL_TEST_SESSION_H_
#define _PASSENGER_APPLICATION_POOL_TEST_SESSION_H_

#include <boost/thread.hpp>
#include <string>
#include <cassert>
#include <Utils/IOUtils.h>
#include <Utils/BufferedIO.h>
#include <Core/ApplicationPool/AbstractSession.h>

namespace Passenger {
namespace ApplicationPool2 {

using namespace std;


class TestSession: public AbstractSession {
private:
	mutable boost::mutex syncher;
	mutable unsigned int refcount;
	pid_t pid;
	string gupid;
	string protocol;
	ApiKey apiKey;
	SocketPair connection;
	BufferedIO peerBufferedIO;
	unsigned int stickySessionId;
	mutable bool closed;
	mutable bool success;
	mutable bool wantKeepAlive;

public:
	TestSession()
		: refcount(1),
		  pid(123),
		  gupid("gupid-123"),
		  protocol("session"),
		  stickySessionId(0),
		  closed(false),
		  success(false),
		  wantKeepAlive(false)
		{ }

	virtual void ref() const {
		boost::lock_guard<boost::mutex> l(syncher);
		assert(refcount > 0);
		refcount++;
	}

	virtual void unref() const {
		boost::lock_guard<boost::mutex> l(syncher);
		assert(refcount > 0);
		refcount--;
		if (refcount == 0) {
			if (!closed) {
				closed = true;
				success = false;
				wantKeepAlive = false;
			}
		}
	}

	virtual pid_t getPid() const {
		boost::lock_guard<boost::mutex> l(syncher);
		return pid;
	}

	void setPid(pid_t p) {
		boost::lock_guard<boost::mutex> l(syncher);
		pid = p;
	}

	virtual StaticString getGupid() const {
		boost::lock_guard<boost::mutex> l(syncher);
		return gupid;
	}

	void setGupid(const string &v) {
		boost::lock_guard<boost::mutex> l(syncher);
		gupid = v;
	}

	virtual StaticString getProtocol() const {
		boost::lock_guard<boost::mutex> l(syncher);
		return protocol;
	}

	void setProtocol(const string &v) {
		boost::lock_guard<boost::mutex> l(syncher);
		protocol = v;
	}

	virtual unsigned int getStickySessionId() const {
		boost::lock_guard<boost::mutex> l(syncher);
		return stickySessionId;
	}

	void setStickySessionId(unsigned int v) {
		boost::lock_guard<boost::mutex> l(syncher);
		stickySessionId = v;
	}

	virtual const ApiKey &getApiKey() const {
		return apiKey;
	}

	virtual int fd() const {
		boost::lock_guard<boost::mutex> l(syncher);
		return connection.first;
	}

	virtual int peerFd() const {
		boost::lock_guard<boost::mutex> l(syncher);
		return connection.second;
	}

	virtual BufferedIO &getPeerBufferedIO() {
		boost::lock_guard<boost::mutex> l(syncher);
		return peerBufferedIO;
	}

	virtual bool isClosed() const {
		boost::lock_guard<boost::mutex> l(syncher);
		return closed;
	}

	bool isSuccessful() const {
		boost::lock_guard<boost::mutex> l(syncher);
		return success;
	}

	bool wantsKeepAlive() const {
		boost::lock_guard<boost::mutex> l(syncher);
		return wantKeepAlive;
	}

	virtual void initiate(bool blocking = true) {
		boost::lock_guard<boost::mutex> l(syncher);
		connection = createUnixSocketPair(__FILE__, __LINE__);
		peerBufferedIO = BufferedIO(connection.second);
		if (!blocking) {
			setNonBlocking(connection.first);
		}
	}

	virtual void close(bool _success, bool _wantKeepAlive = false) {
		boost::lock_guard<boost::mutex> l(syncher);
		closed = true;
		success = _success;
		wantKeepAlive = _wantKeepAlive;
	}

	void closePeerFd() {
		boost::lock_guard<boost::mutex> l(syncher);
		connection.second.close();
	}
};


} // namespace ApplicationPool2
} // namespace Passenger

#endif /* _PASSENGER_APPLICATION_POOL2_TEST_SESSION_H_ */
