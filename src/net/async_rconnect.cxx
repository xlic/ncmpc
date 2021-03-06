/* ncmpc (Ncurses MPD Client)
   (c) 2004-2018 The Music Player Daemon Project
   Project homepage: http://musicpd.org

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "async_rconnect.hxx"
#include "async_connect.hxx"
#include "resolver.hxx"
#include "util/Compiler.h"

#include <string>

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct async_rconnect {
	const struct async_rconnect_handler *handler;
	void *handler_ctx;

	const char *host;
	struct resolver *resolver;

	struct async_connect *connect = nullptr;

	std::string last_error;

	async_rconnect(const char *_host, struct resolver *_resolver,
		       const struct async_rconnect_handler &_handler,
		       void *_ctx)
		:handler(&_handler), handler_ctx(_ctx),
		 host(_host), resolver(_resolver) {}

	~async_rconnect() {
		if (connect != nullptr)
			async_connect_cancel(connect);
		resolver_free(resolver);
	}
};

static void
async_rconnect_next(struct async_rconnect *rc);

static void
async_rconnect_success(socket_t fd, void *ctx)
{
	auto *rc = (struct async_rconnect *)ctx;
	rc->connect = nullptr;

	rc->handler->success(fd, rc->handler_ctx);

	delete rc;
}

static void
async_rconnect_error(const char *message, void *ctx)
{
	auto *rc = (struct async_rconnect *)ctx;
	rc->connect = nullptr;

	rc->last_error = message;

	async_rconnect_next(rc);
}

static const struct async_connect_handler async_rconnect_connect_handler = {
	.success = async_rconnect_success,
	.error = async_rconnect_error,
};

static void
async_rconnect_next(struct async_rconnect *rc)
{
	assert(rc->connect == nullptr);

	const struct resolver_address *a = resolver_next(rc->resolver);
	if (a == nullptr) {
		char msg[256];

		if (rc->last_error.empty()) {
			snprintf(msg, sizeof(msg),
				 "Host '%s' has no address",
				 rc->host);
		} else {
			snprintf(msg, sizeof(msg),
				 "Failed to connect to host '%s': %s",
				 rc->host, rc->last_error.c_str());
		}

		rc->handler->error(msg, rc->handler_ctx);
		delete rc;
		return;
	}

	async_connect_start(&rc->connect, a->addr, a->addrlen,
			    &async_rconnect_connect_handler, rc);
}

void
async_rconnect_start(struct async_rconnect **rcp,
		     const char *host, unsigned port,
		     const struct async_rconnect_handler *handler, void *ctx)
{
	struct resolver *r = resolver_new(host, port);
	if (host == nullptr)
		host = "[default]";

	if (r == nullptr) {
		char msg[256];
		snprintf(msg, sizeof(msg), "Failed to resolve host '%s'",
			 host);
		handler->error(msg, ctx);
		return;
	}

	auto *rc = new async_rconnect(host, r, *handler, ctx);
	*rcp = rc;

	async_rconnect_next(rc);
}

void
async_rconnect_cancel(struct async_rconnect *rc)
{
	delete rc;
}
