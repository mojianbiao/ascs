/*
 * tcp.h
 *
 *  Created on: 2016-7-30
 *      Author: youngwolf
 *		email: mail2tao@163.com
 *		QQ: 676218192
 *		Community on QQ: 198941541
 *
 * TCP related conveniences.
 */

#ifndef _ASCS_EXT_TCP_H_
#define _ASCS_EXT_TCP_H_

#include "packer.h"
#include "unpacker.h"
#include "../tcp/client_socket.h"
#include "../tcp/proxy/socks.h"
#include "../tcp/client.h"
#include "../tcp/server_socket.h"
#include "../tcp/server.h"
#include "../single_service_pump.h"

#ifndef ASCS_DEFAULT_PACKER
#define ASCS_DEFAULT_PACKER ascs::ext::packer<>
#endif

#ifndef ASCS_DEFAULT_UNPACKER
#define ASCS_DEFAULT_UNPACKER ascs::ext::unpacker<>
#endif

namespace ascs { namespace ext { namespace tcp {

typedef ascs::tcp::client_socket_base<ASCS_DEFAULT_PACKER, ASCS_DEFAULT_UNPACKER> client_socket;
typedef client_socket connector;
typedef ascs::tcp::single_client_base<client_socket> single_client;
typedef ascs::tcp::multi_client_base<client_socket> multi_client;
typedef multi_client client;

typedef ascs::tcp::server_socket_base<ASCS_DEFAULT_PACKER, ASCS_DEFAULT_UNPACKER> server_socket;
template<typename Server = ascs::tcp::i_server> using server_socket2 = ascs::tcp::server_socket_base<ASCS_DEFAULT_PACKER, ASCS_DEFAULT_UNPACKER, Server>;
typedef ascs::tcp::server_base<server_socket> server;

#ifdef ASIO_HAS_LOCAL_SOCKETS
typedef ascs::tcp::unix_client_socket_base<ASCS_DEFAULT_PACKER, ASCS_DEFAULT_UNPACKER> unix_client_socket;
typedef ascs::tcp::single_client_base<unix_client_socket> unix_single_client;
typedef ascs::tcp::multi_client_base<unix_client_socket> unix_multi_client;

typedef ascs::tcp::unix_server_socket_base<ASCS_DEFAULT_PACKER, ASCS_DEFAULT_UNPACKER> unix_server_socket;
template<typename Server = ascs::tcp::i_server> using unix_server_socket2 = ascs::tcp::unix_server_socket_base<ASCS_DEFAULT_PACKER, ASCS_DEFAULT_UNPACKER, Server>;
typedef ascs::tcp::unix_server_base<unix_server_socket> unix_server;
#endif

namespace proxy {

namespace socks4 {
	typedef ascs::tcp::proxy::socks4::client_socket_base<ASCS_DEFAULT_PACKER, ASCS_DEFAULT_UNPACKER> client_socket;
	typedef client_socket connector;
	typedef ascs::tcp::single_client_base<client_socket> single_client;
	typedef ascs::tcp::multi_client_base<client_socket> multi_client;
	typedef multi_client client;
}

namespace socks5 {
	typedef ascs::tcp::proxy::socks5::client_socket_base<ASCS_DEFAULT_PACKER, ASCS_DEFAULT_UNPACKER> client_socket;
	typedef client_socket connector;
	typedef ascs::tcp::single_client_base<client_socket> single_client;
	typedef ascs::tcp::multi_client_base<client_socket> multi_client;
	typedef multi_client client;
}

}

}}} //namespace

#endif /* _ASCS_EXT_TCP_H_ */
