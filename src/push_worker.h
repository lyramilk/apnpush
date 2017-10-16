#ifndef _mpush_push_worker_h_
#define _mpush_push_worker_h_
#include "config.h"
#include "redis.h"
#include <libmilk/aio.h>
#include <libmilk/var.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <nghttp2/nghttp2.h>

extern int threadcount;
extern int concurrency;
extern lyramilk::data::string redis_host;
extern unsigned short redis_port;
extern lyramilk::data::string redis_password;
extern lyramilk::data::string apns_host;
extern unsigned short apns_port;
extern lyramilk::data::string apns_topic;
extern lyramilk::data::string apns_cert;
extern lyramilk::data::string apns_cert_password;
extern lyramilk::data::string log_file;

class aioapnspusher:public lyramilk::io::aioselector
{
	lyramilk::io::native_filedescriptor_type sock;
	SSL_CTX* sslctx;
	SSL* sslobj;
	lyramilk::data::var::map responseheader;
	lyramilk::data::string body;
  public:
	nghttp2_session_callbacks *h2_cbks;
	nghttp2_session *h2_session;
	int flag;
	lyramilk::data::stringstream payload;
	lyramilk::teapoy::redis::redis_client redis;

	aioapnspusher();
	virtual ~aioapnspusher();
	virtual bool open(lyramilk::data::string host,lyramilk::data::uint16 port);
	bool init_ssl(lyramilk::data::string keyfilename,lyramilk::data::string passwd);
	virtual void ondestory();
	virtual bool notify_in();
	virtual bool notify_out();
	virtual bool notify_hup();
	virtual bool notify_err();
	virtual bool notify_pri();
	virtual bool notify_attach(lyramilk::io::aiopoll* container);
	virtual bool notify_detach(lyramilk::io::aiopoll* container);
	virtual lyramilk::io::native_filedescriptor_type getfd();

	// nghttp2 callbacks
	static int select_next_proto_cb(SSL *ssl, unsigned char **out,unsigned char *outlen, const unsigned char *in,unsigned int inlen, void *arg);
	static ssize_t send_callback(nghttp2_session *session, const uint8_t *data,size_t length, int flags, void *user_data);
	static ssize_t recv_callback(nghttp2_session *session, uint8_t *buf,size_t length, int flags, void *user_data);
	static int on_frame_send_callback(nghttp2_session *session,const nghttp2_frame *frame,void *user_data);
	static int on_frame_recv_callback(nghttp2_session *session,const nghttp2_frame *frame,void *user_data);
	static int on_header_callback(nghttp2_session *session,const nghttp2_frame *frame,const uint8_t *name, size_t namelen,const uint8_t *value, size_t valuelen,uint8_t flags, void *user_data);
	static int on_begin_headers_callback(nghttp2_session *session,const nghttp2_frame *frame,void *user_data);
	static int on_stream_close_callback(nghttp2_session *session, int32_t stream_id,uint32_t error_code,void *user_data);
	static int on_data_chunk_recv_callback(nghttp2_session *session,uint8_t flags, int32_t stream_id,const uint8_t *data, size_t len,void *user_data);
	static ssize_t data_prd_read_callback(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length,uint32_t *data_flags, nghttp2_data_source *source, void *user_data);
};
#endif
