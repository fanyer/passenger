// This file is included inside the RequestHandler class.

private:

void
sendHeaderToApp(Client *client, Request *req) {
	SKC_TRACE(client, 2, "Sending headers to application with " <<
		req->session->getProtocol() << " protocol");
	req->state = Request::SENDING_HEADER_TO_APP;

	/**
	 * HTTP does not formally support half-closing, and Node.js treats a
	 * half-close as a full close, so we only half-close session sockets, not
	 * HTTP sockets.
	 */
	if (req->session->getProtocol() == "session") {
		req->halfCloseAppConnection = true;
		sendHeaderToAppWithSessionProtocol(client, req);
	} else {
		req->halfCloseAppConnection = false;
		sendHeaderToAppWithHttpProtocol(client, req);
	}

	if (!req->ended()) {
		if (!req->appInput.ended()) {
			if (!req->appInput.passedThreshold()) {
				sendBodyToApp(client, req);
				req->appOutput.startReading();
			} else {
				SKC_TRACE(client, 3, "Waiting for appInput buffers to be "
					"flushed before sending body to application");
				req->appInput.setBuffersFlushedCallback(sendBodyToAppWhenBuffersFlushed);
				req->appOutput.startReading();
			}
		} else {
			// req->appInput.feed() encountered an error while writing to the
			// application socket. But we don't care about that; we just care that
			// ForwardResponse.cpp will now forward the response data and end the
			// request.
			req->state = Request::WAITING_FOR_APP_OUTPUT;
			req->appOutput.startReading();
		}
	}
}

struct SessionProtocolWorkingState {
	const LString *httpPath;
	StaticString path;
	StaticString queryString;
	StaticString methodStr;
	StaticString serverName;
	StaticString serverPort;
};

void
sendHeaderToAppWithSessionProtocol(Client *client, Request *req) {
	SessionProtocolWorkingState state;
	unsigned int bufferSize = determineHeaderSizeForSessionProtocol(req,
		state);
	MemoryKit::mbuf_pool &mbuf_pool = getContext()->mbuf_pool;
	bool ok;

	if (bufferSize <= mbuf_pool.mbuf_block_chunk_size) {
		MemoryKit::mbuf buffer(MemoryKit::mbuf_get(&mbuf_pool));
		bufferSize = mbuf_pool.mbuf_block_chunk_size;

		ok = constructHeaderForSessionProtocol(req, buffer.start,
			bufferSize, state);
		assert(ok);
		buffer = MemoryKit::mbuf(buffer, 0, bufferSize);
		SKC_TRACE(client, 3, "Header data: \"" << cEscapeString(
			StaticString(buffer.start, bufferSize)) << "\"");
		req->appInput.feed(buffer);
	} else {
		char *buffer = (char *) psg_pnalloc(req->pool, bufferSize);

		ok = constructHeaderForSessionProtocol(req, buffer,
			bufferSize, state);
		assert(ok);
		SKC_TRACE(client, 3, "Header data: \"" << cEscapeString(
			StaticString(buffer, bufferSize)) << "\"");
		req->appInput.feed(buffer, bufferSize);
	}

	(void) ok; // Shut up compiler warning
}

static void
sendBodyToAppWhenBuffersFlushed(FileBufferedChannel *_channel) {
	FileBufferedFdOutputChannel *channel =
		reinterpret_cast<FileBufferedFdOutputChannel *>(_channel);
	Request *req = static_cast<Request *>(static_cast<
		ServerKit::BaseHttpRequest *>(channel->getHooks()->userData));
	Client *client = static_cast<Client *>(req->client);
	RequestHandler *self = static_cast<RequestHandler *>(
		getServerFromClient(client));

	req->appInput.setBuffersFlushedCallback(NULL);
	self->sendBodyToApp(client, req);
}

unsigned int
determineHeaderSizeForSessionProtocol(Request *req,
	SessionProtocolWorkingState &state)
{
	unsigned int dataSize = sizeof(boost::uint32_t);
	const char *queryStringStart;

	state.httpPath = psg_lstr_make_contiguous(&req->path, req->pool);
	queryStringStart = (const char *) memchr(
		state.httpPath->start->data, '?', state.httpPath->size);
	if (queryStringStart != NULL) {
		state.path = StaticString(state.httpPath->start->data,
			queryStringStart - state.httpPath->start->data);
		state.queryString = StaticString(queryStringStart,
			state.httpPath->start->data + state.httpPath->size - queryStringStart);
	} else {
		state.path = StaticString(state.httpPath->start->data, state.httpPath->size);
	}

	dataSize += sizeof("REQUEST_URI");
	dataSize += req->path.size + 1;

	dataSize += sizeof("PATH_INFO");
	dataSize += state.path.size() + 1;

	dataSize += sizeof("SCRIPT_NAME");
	dataSize += sizeof("");

	dataSize += sizeof("QUERY_STRING");
	dataSize += state.queryString.size() + 1;

	state.methodStr = StaticString(http_method_str(req->method));
	dataSize += sizeof("REQUEST_METHOD");
	dataSize += state.methodStr.size() + 1;

	if (req->host != NULL) {
		const LString *host = psg_lstr_make_contiguous(req->host, req->pool);
		const char *sep = (const char *) memchr(host->start->data, ':', host->size);
		if (sep != NULL) {
			state.serverName = StaticString(host->start->data, sep - host->start->data);
			state.serverPort = StaticString(sep + 1,
				host->start->data + host->size - sep - 1);
		} else {
			state.serverName = StaticString(host->start->data, host->size);
			state.serverPort = P_STATIC_STRING("80");
		}
	} else {
		state.serverName = defaultServerName;
		state.serverPort = defaultServerPort;
	}

	dataSize += sizeof("SERVER_NAME");
	dataSize += state.serverName.size() + 1;

	dataSize += sizeof("SERVER_PORT");
	dataSize += state.serverPort.size() + 1;

	dataSize += sizeof("PASSENGER_CONNECT_PASSWORD");
	dataSize += req->session->getGroupSecret().size() + 1;

	if (req->https) {
		dataSize += sizeof("HTTPS");
		dataSize += sizeof("on");
	}

	if (req->options.analytics) {
		dataSize += sizeof("PASSENGER_TXN_ID");
		dataSize += req->options.transaction->getTxnId().size() + 1;
	}

	ServerKit::HeaderTable::Iterator it(req->headers);
	while (*it != NULL) {
		dataSize += sizeof("HTTP_") - 1 + it->header->key.size + 1;
		dataSize += it->header->val.size + 1;
		it.next();
	}

	return dataSize + 1;
}

bool
constructHeaderForSessionProtocol(Request *req, char * restrict buffer, unsigned int &size,
	const SessionProtocolWorkingState &state)
{
	char *pos = buffer;
	const char *end = buffer + size;
	const LString *value;
	const LString::Part *part;

	pos += sizeof(boost::uint32_t);

	pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("REQUEST_URI"));
	pos = appendData(pos, end, state.httpPath->start->data, state.httpPath->size);
	pos = appendData(pos, end, "", 1);

	pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("PATH_INFO"));
	pos = appendData(pos, end, state.path.data(), state.path.size());
	pos = appendData(pos, end, "", 1);

	pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("SCRIPT_NAME"));
	pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL(""));

	pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("QUERY_STRING"));
	pos = appendData(pos, end, state.queryString.data(), state.queryString.size());
	pos = appendData(pos, end, "", 1);

	pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("REQUEST_METHOD"));
	pos = appendData(pos, end, state.methodStr);
	pos = appendData(pos, end, "", 1);

	pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("SERVER_NAME"));
	pos = appendData(pos, end, state.serverName);
	pos = appendData(pos, end, "", 1);

	pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("SERVER_PORT"));
	pos = appendData(pos, end, state.serverPort);
	pos = appendData(pos, end, "", 1);

	value = req->headers.lookup(HTTP_CONTENT_LENGTH);
	if (value != NULL) {
		pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("CONTENT_LENGTH"));
		part = value->start;
		while (part != NULL) {
			pos = appendData(pos, end, part->data, part->size);
			part = part->next;
		}
		pos = appendData(pos, end, "", 1);
	}

	pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("PASSENGER_CONNECT_PASSWORD"));
	pos = appendData(pos, end, req->session->getGroupSecret());
	pos = appendData(pos, end, "", 1);

	if (req->https) {
		pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("HTTPS"));
		pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("on"));
	}

	if (req->options.analytics) {
		pos = appendData(pos, end, P_STATIC_STRING_WITH_NULL("PASSENGER_TXN_ID"));
		pos = appendData(pos, end, req->options.transaction->getTxnId());
		pos = appendData(pos, end, "", 1);
	}

	ServerKit::HeaderTable::Iterator it(req->headers);
	while (*it != NULL) {
		if ((it->header->hash == HTTP_CONTENT_TYPE_HASH
			|| it->header->hash == HTTP_CONTENT_LENGTH.hash()
			|| it->header->hash == HTTP_CONNECTION.hash())
		 && (psg_lstr_cmp(&it->header->key, P_STATIC_STRING("content-type"))
			|| psg_lstr_cmp(&it->header->key, P_STATIC_STRING("content-length"))
			|| psg_lstr_cmp(&it->header->key, P_STATIC_STRING("connection"))))
		{
			it.next();
			continue;
		}

		pos = appendData(pos, end, P_STATIC_STRING("HTTP_"));
		const LString::Part *part = it->header->key.start;
		while (part != NULL) {
			char *start = pos;
			pos = appendData(pos, end, part->data, part->size);
			httpHeaderToScgiUpperCase((unsigned char *) start, pos - start);
			part = part->next;
		}
		pos = appendData(pos, end, "", 1);

		part = it->header->val.start;
		while (part != NULL) {
			pos = appendData(pos, end, part->data, part->size);
			part = part->next;
		}
		pos = appendData(pos, end, "", 1);

		it.next();
	}

	Uint32Message::generate(buffer, pos - buffer - sizeof(boost::uint32_t));

	size = pos - buffer;
	return pos < end;
}

void
httpHeaderToScgiUpperCase(unsigned char *data, unsigned int size) {
	static const boost::uint8_t toUpperMap[256] = {
		'\0', 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, '\t',
		'\n', 0x0b, 0x0c, '\r', 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13,
		0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d,
		0x1e, 0x1f,  ' ',  '!',  '"',  '#',  '$',  '%',  '&', '\'',
		 '(',  ')',  '*',  '+',  ',',  '_',  '.',  '/',  '0',  '1',
		 '2',  '3',  '4',  '5',  '6',  '7',  '8',  '9',  ':',  ';',
		 '<',  '=',  '>',  '?',  '@',  'A',  'B',  'C',  'D',  'E',
		 'F',  'G',  'H',  'I',  'J',  'K',  'L',  'M',  'N',  'O',
		 'P',  'Q',  'R',  'S',  'T',  'U',  'V',  'W',  'X',  'Y',
		 'Z',  '[', '\\',  ']',  '^',  '_',  '`',  'A',  'B',  'C',
		 'D',  'E',  'F',  'G',  'H',  'I',  'J',  'K',  'L',  'M',
		 'N',  'O',  'P',  'Q',  'R',  'S',  'T',  'U',  'V',  'W',
		 'X',  'Y',  'Z',  '{',  '|',  '}',  '~', 0x7f, 0x80, 0x81,
		0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b,
		0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95,
		0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
		0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
		0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3,
		0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd,
		0xbe, 0xbf, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
		0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xd0, 0xd1,
		0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb,
		0xdc, 0xdd, 0xde, 0xdf, 0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5,
		0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
		0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9,
		0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff
	};

	const unsigned char *buf = data;
	const size_t imax = size / 8;
	const size_t leftover = size % 8;
	size_t i;

	for (i = 0; i < imax; i++, data += 8) {
		data[0] = (unsigned char) toUpperMap[data[0]];
		data[1] = (unsigned char) toUpperMap[data[1]];
		data[2] = (unsigned char) toUpperMap[data[2]];
		data[3] = (unsigned char) toUpperMap[data[3]];
		data[4] = (unsigned char) toUpperMap[data[4]];
		data[5] = (unsigned char) toUpperMap[data[5]];
		data[6] = (unsigned char) toUpperMap[data[6]];
		data[7] = (unsigned char) toUpperMap[data[7]];
	}

	i = imax * 8;
	switch (leftover) {
	case 7: *data++ = (unsigned char) toUpperMap[buf[i++]];
	case 6: *data++ = (unsigned char) toUpperMap[buf[i++]];
	case 5: *data++ = (unsigned char) toUpperMap[buf[i++]];
	case 4: *data++ = (unsigned char) toUpperMap[buf[i++]];
	case 3: *data++ = (unsigned char) toUpperMap[buf[i++]];
	case 2: *data++ = (unsigned char) toUpperMap[buf[i++]];
	case 1: *data++ = (unsigned char) toUpperMap[buf[i]];
	case 0: break;
	}
}

void
sendHeaderToAppWithHttpProtocol(Client *client, Request *req) {
	ssize_t bytesWritten;

	if (!sendHeaderToAppWithHttpProtocolAndWritev(req, bytesWritten)) {
		if (bytesWritten >= 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
			sendHeaderToAppWithSessionProtocolWithBuffering(req, bytesWritten);
		} else {
			int e = errno;
			assert(bytesWritten == -1);
			onAppInputError(NULL, e);
		}
	}
}

/**
 * Construct an array of buffers, which together contain the 'http' protocol header
 * data that should be sent to the application. This method does not copy any data:
 * it just constructs buffers that point to the data stored inside `req->pool`,
 * `req->headers`, etc.
 *
 * The buffers will be stored in the array pointed to by `buffer`. This array must
 * have space for at least `maxbuffers` items. The actual number of buffers constructed
 * is stored in `nbuffers`, and the total data size of the buffers is stored in `dataSize`.
 * Upon success, returns true. If the actual number of buffers necessary exceeds
 * `maxbuffers`, then false is returned.
 *
 * You can also set `buffers` to NULL, in which case this method will not construct any
 * buffers, but only count the number of buffers necessary, as well as the total data size.
 * In this case, this method always returns true.
 */
bool
constructHeaderBuffersForHttpProtocol(Request *req, struct iovec *buffers,
	unsigned int maxbuffers, unsigned int & restrict_ref nbuffers,
	unsigned int & restrict_ref dataSize)
{
	#define INC_BUFFER_ITER(i) \
		do { \
			if (buffers != NULL && i == maxbuffers) { \
				return false; \
			} \
			i++; \
		} while (false)

	ServerKit::HeaderTable::Iterator it(req->headers);
	const LString *value;
	const LString::Part *part;
	const char *methodStr;
	size_t methodStrLen;
	unsigned int i = 0;

	nbuffers = 0;
	dataSize = 0;

	methodStr    = http_method_str(req->method);
	methodStrLen = strlen(methodStr);
	if (buffers != NULL) {
		buffers[i].iov_base = (void *) methodStr;
		buffers[i].iov_len  = methodStrLen;
	}
	INC_BUFFER_ITER(i);
	dataSize += sizeof(methodStrLen);

	if (buffers != NULL) {
		buffers[i].iov_base = (void *) " ";
		buffers[i].iov_len  = 1;
	}
	INC_BUFFER_ITER(i);
	dataSize += 1;

	part = req->path.start;
	while (part != NULL) {
		if (buffers != NULL) {
			buffers[i].iov_base = (void *) part->data;
			buffers[i].iov_len  = part->size;
		}
		INC_BUFFER_ITER(i);
		part = part->next;
	}
	dataSize += req->path.size;

	buffers[i].iov_base = (void *) " HTTP/1.1\r\n";
	buffers[i].iov_len  = sizeof(" HTTP/1.1\r\n") - 1;
	INC_BUFFER_ITER(i);
	dataSize += sizeof(" HTTP/1.1\r\n") - 1;

	while (*it != NULL) {
		dataSize += it->header->key.size + sizeof(": ") - 1;
		dataSize += it->header->val.size + sizeof("\r\n") - 1;

		part = it->header->key.start;
		while (part != NULL) {
			if (buffers != NULL) {
				buffers[i].iov_base = (void *) part->data;
				buffers[i].iov_len  = part->size;
			}
			INC_BUFFER_ITER(i);
			part = part->next;
		}
		if (buffers != NULL) {
			buffers[i].iov_base = (void *) ": ";
			buffers[i].iov_len  = sizeof(": ") - 1;
		}
		INC_BUFFER_ITER(i);

		part = it->header->val.start;
		while (part != NULL) {
			if (buffers != NULL) {
				buffers[i].iov_base = (void *) part->data;
				buffers[i].iov_len  = part->size;
			}
			INC_BUFFER_ITER(i);
			part = part->next;
		}
		if (buffers != NULL) {
			buffers[i].iov_base = (void *) "\r\n";
			buffers[i].iov_len  = sizeof("\r\n") - 1;
		}
		INC_BUFFER_ITER(i);

		it.next();
	}

	if (req->https) {
		if (buffers != NULL) {
			buffers[i].iov_base = (void *) "X-Forwarded-Proto: https\r\n";
			buffers[i].iov_len  = sizeof("X-Forwarded-Proto: https\r\n") - 1;
		}
		INC_BUFFER_ITER(i);
		dataSize += sizeof("X-Forwarded-Proto: https\r\n") - 1;
	}

	value = req->secureHeaders.lookup(REMOTE_ADDR);
	if (value != NULL && value->size > 0) {
		dataSize += (sizeof("X-Forwarded-Proto: ") - 1) + (sizeof("\r\n") - 1);

		if (buffers != NULL) {
			buffers[i].iov_base = (void *) "X-Forwarded-For: ";
			buffers[i].iov_len  = sizeof("X-Forwarded-For: ") - 1;
		}
		INC_BUFFER_ITER(i);

		part = value->start;
		while (part != NULL) {
			if (buffers != NULL) {
				buffers[i].iov_base = (void *) part->data;
				buffers[i].iov_len  = part->size;
			}
			INC_BUFFER_ITER(i);
			part = part->next;
		}

		if (buffers != NULL) {
			buffers[i].iov_base = (void *) "\r\n";
			buffers[i].iov_len  = sizeof("\r\n") - 1;
		}
		INC_BUFFER_ITER(i);
	}

	if (req->options.analytics) {
		if (buffers != NULL) {
			buffers[i].iov_base = (void *) "Passenger-Txn-Id: ";
			buffers[i].iov_len  = sizeof("Passenger-Txn-Id: ") - 1;
		}
		INC_BUFFER_ITER(i);
		dataSize += sizeof("Passenger-Txn-Id: ") - 1;

		if (buffers != NULL) {
			buffers[i].iov_base = (void *) req->options.transaction->getTxnId().data();
			buffers[i].iov_len  = req->options.transaction->getTxnId().size();
		}
		INC_BUFFER_ITER(i);
		dataSize += req->options.transaction->getTxnId().size();

		if (buffers != NULL) {
			buffers[i].iov_base = (void *) "\r\n";
			buffers[i].iov_len  = sizeof("\r\n") - 1;
		}
		INC_BUFFER_ITER(i);
	}

	if (buffers != NULL) {
		buffers[i].iov_base = (void *) "\r\n";
		buffers[i].iov_len  = sizeof("\r\n") - 1;
	}
	INC_BUFFER_ITER(i);
	dataSize += sizeof("\r\n") - 1;

	nbuffers = i;
	return true;

	#undef INC_BUFFER_ITER
}

bool
sendHeaderToAppWithHttpProtocolAndWritev(Request *req, ssize_t &bytesWritten) {
	unsigned int maxbuffers = std::min<unsigned int>(
		4 + req->headers.size() * 4 + 4, IOV_MAX);
	struct iovec *buffers = (struct iovec *) psg_palloc(req->pool,
		sizeof(struct iovec) * maxbuffers);
	unsigned int nbuffers, dataSize;

	if (constructHeaderBuffersForHttpProtocol(req, buffers,
		maxbuffers, nbuffers, dataSize))
	{
		ssize_t ret;
		do {
			ret = writev(req->session->fd(), buffers, nbuffers);
		} while (ret == -1 && errno == EINTR);
		bytesWritten = ret;
		return ret == (ssize_t) dataSize;
	} else {
		bytesWritten = 0;
		return false;
	}
}

void
sendHeaderToAppWithSessionProtocolWithBuffering(Request *req, unsigned int offset) {
	struct iovec *buffers;
	unsigned int nbuffers, dataSize;
	bool ok;

	ok = constructHeaderBuffersForHttpProtocol(req, NULL, 0, nbuffers, dataSize);
	assert(ok);

	buffers = (struct iovec *) psg_palloc(req->pool,
		sizeof(struct iovec) * nbuffers);
	ok = constructHeaderBuffersForHttpProtocol(req, buffers, nbuffers,
		nbuffers, dataSize);
	assert(ok);
	(void) ok; // Shut up compiler warning

	MemoryKit::mbuf_pool &mbuf_pool = getContext()->mbuf_pool;
	if (dataSize <= mbuf_pool.mbuf_block_chunk_size) {
		MemoryKit::mbuf buffer(MemoryKit::mbuf_get(&mbuf_pool));
		gatherBuffers(buffer.start, mbuf_pool.mbuf_block_chunk_size,
			buffers, nbuffers);
		buffer = MemoryKit::mbuf(buffer, offset, dataSize - offset);
		req->appInput.feed(boost::move(buffer));
	} else {
		char *buffer = (char *) psg_pnalloc(req->pool, dataSize);
		gatherBuffers(buffer, dataSize, buffers, nbuffers);
		req->appInput.feed(buffer + offset, dataSize - offset);
	}
}

void
sendBodyToApp(Client *client, Request *req) {
	if (req->hasBody() || req->upgraded()) {
		// onRequestBody() will take care of forwarding
		// the request body to the app.
		SKC_TRACE(client, 2, "Sending body to application");
		req->state = Request::FORWARDING_BODY_TO_APP;
		req->bodyChannel.start();
	} else {
		// Our task is done. ForwardResponse.cpp will take
		// care of ending the request, once all response
		// data is forwarded.
		SKC_TRACE(client, 2, "No body to send to application");
		req->state = Request::WAITING_FOR_APP_OUTPUT;
		maybeHalfCloseAppInput(client, req);
	}
}


protected:

virtual Channel::Result
onRequestBody(Client *client, Request *req, const MemoryKit::mbuf &buffer,
	int errcode)
{
	assert(req->state == Request::FORWARDING_BODY_TO_APP);

	if (buffer.size() > 0) {
		// Data
		SKC_TRACE(client, 3, "Forwarding " << buffer.size() <<
			" bytes of client request body: \"" <<
			cEscapeString(StaticString(buffer.start, buffer.size())) <<
			"\"");
		req->appInput.feed(buffer);
		if (!req->appInput.ended()) {
			if (req->appInput.passedThreshold()) {
				req->bodyChannel.stop();
				req->appInput.setBuffersFlushedCallback(resumeRequestBodyChannelWhenBuffersFlushed);
			}
			return Channel::Result(buffer.size(), false);
		} else {
			return Channel::Result(buffer.size(), true);
		}
	} else if (errcode == 0 || errcode == ECONNRESET) {
		// EOF
		SKC_TRACE(client, 2, "Client sent EOF");
		// Our task is done. ForwardResponse.cpp will take
		// care of ending the request, once all response
		// data is forwarded.
		req->state = Request::WAITING_FOR_APP_OUTPUT;
		maybeHalfCloseAppInput(client, req);
		return Channel::Result(0, true);
	} else {
		const unsigned int BUFSIZE = 1024;
		char *message = (char *) psg_pnalloc(req->pool, BUFSIZE);
		int size = snprintf(message, BUFSIZE,
			"error reading request body: %s (errno=%d)",
			ServerKit::getErrorDesc(errcode), errcode);
		disconnectWithError(&client, StaticString(message, size));
		return Channel::Result(0, true);
	}
}

static void
resumeRequestBodyChannelWhenBuffersFlushed(FileBufferedChannel *_channel) {
	FileBufferedFdOutputChannel *channel =
		reinterpret_cast<FileBufferedFdOutputChannel *>(_channel);
	Request *req = static_cast<Request *>(static_cast<
		ServerKit::BaseHttpRequest *>(channel->getHooks()->userData));

	assert(req->state == Request::FORWARDING_BODY_TO_APP);

	req->appInput.setBuffersFlushedCallback(NULL);
	req->bodyChannel.start();
}

void
maybeHalfCloseAppInput(Client *client, Request *req) {
	assert(req->state == Request::WAITING_FOR_APP_OUTPUT);
	if (req->halfCloseAppConnection) {
		if (!req->appInput.ended()) {
			req->appInput.feed(MemoryKit::mbuf());
		}
		if (req->appInput.endAcked()) {
			halfCloseAppInput(client, req);
		} else {
			req->appInput.setDataFlushedCallback(halfCloseAppInputWhenDataFlushed);
		}
	}
}

void
halfCloseAppInput(Client *client, Request *req) {
	SKC_TRACE(client, 3, "Half-closing application socket with SHUT_WR");
	assert(req->halfCloseAppConnection);
	::shutdown(req->session->fd(), SHUT_WR);
}

static void
halfCloseAppInputWhenDataFlushed(FileBufferedChannel *_channel) {
	FileBufferedFdOutputChannel *channel =
		reinterpret_cast<FileBufferedFdOutputChannel *>(_channel);
	Request *req = static_cast<Request *>(static_cast<
		ServerKit::BaseHttpRequest *>(channel->getHooks()->userData));
	Client *client = static_cast<Client *>(req->client);
	RequestHandler *self = static_cast<RequestHandler *>(
		getServerFromClient(client));

	assert(req->state == Request::WAITING_FOR_APP_OUTPUT);

	req->appInput.setDataFlushedCallback(NULL);
	self->halfCloseAppInput(client, req);
}


private:

static void
onAppInputError(FileBufferedFdOutputChannel *channel, int errcode) {
	Request *req = static_cast<Request *>(static_cast<
		ServerKit::BaseHttpRequest *>(channel->getHooks()->userData));
	Client *client = static_cast<Client *>(req->client);
	RequestHandler *self = static_cast<RequestHandler *>(getServerFromClient(client));

	assert(req->state == Request::SENDING_HEADER_TO_APP
		|| req->state == Request::FORWARDING_BODY_TO_APP
		|| req->state == Request::WAITING_FOR_APP_OUTPUT);

	if (errcode == EPIPE) {
		// We consider an EPIPE non-fatal: we don't care whether the
		// app stopped reading, we just care about its output.
		SKC_DEBUG_FROM_STATIC(self, client, "cannot write to application socket: "
			"the application closed the socket prematurely");
	} else if (req->responseBegun) {
		const unsigned int BUFSIZE = 1024;
		char *message = (char *) psg_pnalloc(req->pool, BUFSIZE);
		int size = snprintf(message, BUFSIZE,
			"cannot write to application socket: %s (errno=%d)",
			ServerKit::getErrorDesc(errcode), errcode);
		self->disconnectWithError(&client, StaticString(message, size));
	} else {
		self->endRequest(&client, &req);
	}
}