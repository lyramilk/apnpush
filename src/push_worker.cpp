#include "push_worker.h"
#include "guid.h"
#include <sys/epoll.h>
#include <libmilk/log.h>
#include <libmilk/multilanguage.h>
#include <libmilk/json.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>

int threadcount = 100;
int concurrency = 1000;
lyramilk::data::string redis_host = "127.0.0.1";
unsigned short redis_port = 6379;
lyramilk::data::string redis_password;
lyramilk::data::string apns_host = "api.push.apple.com";
unsigned short apns_port = 443;
lyramilk::data::string apns_topic;
lyramilk::data::string apns_cert = "./ck_production.pem";
lyramilk::data::string apns_cert_password;
lyramilk::data::string log_file;

struct __ssl
{
	__ssl()
	{
		SSL_library_init();
		SSL_load_error_strings();
		ERR_load_BIO_strings();
		OpenSSL_add_all_algorithms();
	}
	~__ssl()
	{
	}

	lyramilk::data::string err()
	{
		char buff[4096] = {0};
		ERR_error_string(ERR_get_error(),buff);
		return buff;
	}
};

static __ssl _ssl;

aioapnspusher::aioapnspusher()
{
	sock = 0;
	h2_cbks = nullptr;
	h2_session = nullptr;
	sslctx = nullptr;
	sslobj = nullptr;
	flag = EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLONESHOT;
}

aioapnspusher::~aioapnspusher()
{
	if(h2_session){
		nghttp2_session_del(h2_session);
	}
	if(h2_cbks){
		nghttp2_session_callbacks_del(h2_cbks);
	}
	if(sslobj){
		SSL_shutdown(sslobj);
		SSL_free(sslobj);
		SSL_CTX_free(sslctx);
	}
	if(sock){
		::close(sock);
	}
}

bool aioapnspusher::open(lyramilk::data::string host,lyramilk::data::uint16 port)
{
	if(!redis.isalive()){
		while(true){
			try{
				if(redis.open(redis_host,redis_port)){
					if(redis_password.empty() || redis.auth(redis_password)){
						break;
					}
				}
			}catch(...){
			}
			sleep(1);
			lyramilk::klog(lyramilk::log::error,"mpush.aioapnspusher.open") << lyramilk::kdict("重新连接redis(%s:%d)",redis_host.c_str(),redis_port) << std::endl;
		}
	}

	//redis.set_listener(lyramilk::teapoy::redis::redis_client::default_listener);
	if(sock){
		lyramilk::klog(lyramilk::log::error,"mpush.aioapnspusher.open") << lyramilk::kdict("打开监听套件字失败，因为该套接字己打开。") << std::endl;
		return false;
	}
	hostent* h = gethostbyname(host.c_str());
	if(h == nullptr){
		lyramilk::klog(lyramilk::log::error,"mpush.aioapnspusher.open") << lyramilk::kdict("获取IP地址失败：%s",strerror(errno)) << std::endl;
		return false;
	}

	in_addr* inaddr = (in_addr*)h->h_addr;
	if(inaddr == nullptr){
		lyramilk::klog(lyramilk::log::error,"mpush.aioapnspusher.open") << lyramilk::kdict("获取IP地址失败：%s",strerror(errno)) << std::endl;
		return false;
	}

	lyramilk::io::native_filedescriptor_type tmpsock = ::socket(AF_INET,SOCK_STREAM, IPPROTO_IP);
	if(!tmpsock){
		lyramilk::klog(lyramilk::log::error,"mpush.aioapnspusher.open") << lyramilk::kdict("打开监听套件字失败：%s",strerror(errno)) << std::endl;
		return false;
	}

	sockaddr_in addr = {0};
	addr.sin_addr.s_addr = inaddr->s_addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);


	if(0 == ::connect(tmpsock,(const sockaddr*)&addr,sizeof(addr))){
		SSL* sslptr = SSL_new(sslctx);
		if(SSL_set_fd(sslptr,tmpsock) != 1) {
			sslptr = nullptr;
			lyramilk::klog(lyramilk::log::warning,"mpush.aioapnspusher.onevent") << lyramilk::kdict("绑定套接字失败:%s",_ssl.err().c_str()) << std::endl;
			::close(tmpsock);
			return false;
		}

		SSL_set_connect_state(sslptr);
		if(SSL_do_handshake(sslptr) != 1) {
			lyramilk::klog(lyramilk::log::warning,"mpush.aioapnspusher.onevent") << lyramilk::kdict("握手失败:%s",_ssl.err().c_str()) << std::endl;
			::close(tmpsock);
			return false;
		}
		this->sslobj = sslptr;
		unsigned int argp = 1;
		ioctl(tmpsock,FIONBIO,&argp);
		this->sock = tmpsock;



		int r = nghttp2_session_callbacks_new(&h2_cbks);
		if(r != 0){
			lyramilk::klog(lyramilk::log::warning,"mpush.aioapnspusher.onevent") << lyramilk::kdict("初始化nghttp2回调失败") << std::endl;
		}
		nghttp2_session_callbacks_set_send_callback(h2_cbks, send_callback);
		nghttp2_session_callbacks_set_recv_callback(h2_cbks, recv_callback);

		nghttp2_session_callbacks_set_on_frame_send_callback(h2_cbks, on_frame_send_callback);
		nghttp2_session_callbacks_set_on_frame_recv_callback(h2_cbks, on_frame_recv_callback);
		nghttp2_session_callbacks_set_on_header_callback(h2_cbks, on_header_callback);
		nghttp2_session_callbacks_set_on_begin_headers_callback(h2_cbks, on_begin_headers_callback);
		nghttp2_session_callbacks_set_on_stream_close_callback(h2_cbks, on_stream_close_callback);
		nghttp2_session_callbacks_set_on_data_chunk_recv_callback(h2_cbks, on_data_chunk_recv_callback);

		nghttp2_session_client_new(&h2_session,h2_cbks,this);

		nghttp2_submit_settings(h2_session, NGHTTP2_FLAG_NONE, NULL, 0);
		return true;
	}
	::close(tmpsock);
	return false;
}

bool aioapnspusher::init_ssl(lyramilk::data::string keyfilename,lyramilk::data::string passwd)
{
	sslctx = SSL_CTX_new(SSLv23_client_method());
/*
	SSL_CTX_set_options(sslctx, SSL_OP_ALL | SSL_OP_NO_SSLv2);
	SSL_CTX_set_mode(sslctx, SSL_MODE_AUTO_RETRY);*/
	SSL_CTX_set_mode(sslctx, SSL_MODE_RELEASE_BUFFERS);
	SSL_CTX_set_next_proto_select_cb(sslctx, select_next_proto_cb, nullptr);

	int r = 0;
	if(!keyfilename.empty()){
		BIO  *bio = NULL;
		X509 *x509 = NULL;
		if (NULL == (bio = BIO_new_file(keyfilename.c_str(), "r")))
		{
			return NULL;
		}
		x509 = PEM_read_bio_X509_AUX(bio, NULL, NULL, NULL);
		BIO_free(bio);

		r = SSL_CTX_use_certificate(sslctx, x509);
		X509_free(x509);
		if(r != 1) {
			lyramilk::klog(lyramilk::log::warning,"mpush.aioapnspusher.init_ssl") << lyramilk::kdict("设置私钥失败:%s",_ssl.err().c_str()) << std::endl;
			return false;
		}

		SSL_CTX_set_default_passwd_cb_userdata(sslctx, (char*)passwd.c_str());
		if(r != 1) {
			lyramilk::klog(lyramilk::log::warning,"mpush.aioapnspusher.init_ssl") << lyramilk::kdict("设置私钥失败:%s",_ssl.err().c_str()) << std::endl;
			return false;
		}
		r = SSL_CTX_use_PrivateKey_file(sslctx, keyfilename.c_str(), SSL_FILETYPE_PEM);
		if(r != 1) {
			lyramilk::klog(lyramilk::log::warning,"mpush.aioapnspusher.init_ssl") << lyramilk::kdict("设置私钥失败:%s",_ssl.err().c_str()) << std::endl;
			return false;
		}
	}
	if(!keyfilename.empty()){
		r = SSL_CTX_check_private_key(sslctx);
		if(r != 1) {
			lyramilk::klog(lyramilk::log::warning,"mpush.aioapnspusher.init_ssl") << lyramilk::kdict("验证公钥失败:%s",_ssl.err().c_str()) << std::endl;
			return false;
		}
	}
	SSL_CTX_set_options(sslctx, SSL_OP_TLS_ROLLBACK_BUG);
	return true;
}

void aioapnspusher::ondestory()
{
	delete this;
}

bool aioapnspusher::notify_in()
{
	//if(!nghttp2_session_want_read(h2_session)) return pool->reset(this,flag);
	int r = nghttp2_session_recv(h2_session);
	if(r<0){
		lyramilk::klog(lyramilk::log::warning,"mpush.aioapnspusher.notify_in") << lyramilk::kdict("HTTP2错误:%s",nghttp2_strerror(r)) << std::endl;
		return false;
	}
	return notify_out();
}

lyramilk::data::string stroflong(unsigned long long l)
{
	lyramilk::data::stringstream ss;
	ss << l;
	return ss.str();
}

bool aioapnspusher::notify_out()
{
	lyramilk::data::string pushtask;
	{
		lyramilk::data::var vjsonpushtask;
		lyramilk::data::var::array args;
		args.push_back("lpop");
		args.push_back("mpush:queue");
		if(!redis.exec(args,vjsonpushtask) || vjsonpushtask.type() != lyramilk::data::var::t_str){
			sleep(1);
			return pool->reset(this,flag);
		}
		pushtask = vjsonpushtask.str();
	}

	lyramilk::data::var v;
	lyramilk::data::json::parse(pushtask,&v);

	lyramilk::data::string payloadstr = v["data"];
	if(payloadstr.empty()){
		lyramilk::klog(lyramilk::log::warning,"mpush.aioapnspusher.notify_out") << lyramilk::kdict("推送错误:payload为空") << std::endl;
		return pool->reset(this,flag);
	}

	lyramilk::data::string apnsid;
	if(v["apnsid"].type() == lyramilk::data::var::t_invalid){
		{
			lyramilk::data::var ret;
			lyramilk::data::var::array args;
			args.push_back("incrby");
			args.push_back("mpush:index");
			args.push_back(1);
			redis.exec(args,ret);
			if(ret.type() == lyramilk::data::var::t_int){
				unsigned long long i = ret;
				apnsid = guid::str(i);
			}
		}
		{
			lyramilk::data::var ret;
			lyramilk::data::var::array args;
			args.push_back("hset");
			args.push_back("mpush:id");
			args.push_back(apnsid);
			args.push_back(pushtask);
			while(!redis.exec(args,ret));
		}
	}else{
		apnsid = v["apnsid"].str();
		lyramilk::klog(lyramilk::log::warning,"mpush.aioapnspusher.notify_out") << lyramilk::kdict("重新投递") << v << std::endl;
	}

	lyramilk::data::string path = "/3/device/";
	path += v["token"].str();

	payload.clear();
	payload.str("");
	payload << payloadstr;

	std::tr1::unordered_map<lyramilk::data::string,lyramilk::data::string> m;
	m["content-length"] = stroflong(payloadstr.size());
	m["apns-topic"] = apns_topic;
	m["apns-id"] = apnsid;

	std::vector<nghttp2_nv> headers;
	{
		nghttp2_nv nv;
		nv.name = (uint8_t*)":method";
		nv.namelen = 7;
		nv.value = (uint8_t*)"POST";
		nv.valuelen = 4;
		nv.flags = NGHTTP2_NV_FLAG_NONE;
		headers.push_back(nv);
	}
	{
		nghttp2_nv nv;
		nv.name = (uint8_t*)":path";
		nv.namelen = 5;
		nv.value = (uint8_t*)path.c_str();
		nv.valuelen = path.size();
		nv.flags = NGHTTP2_NV_FLAG_NONE;
		headers.push_back(nv);
	}
	std::tr1::unordered_map<lyramilk::data::string,lyramilk::data::string>::iterator it = m.begin();
	for(;it!=m.end();++it){
		nghttp2_nv nv;
		nv.name = (unsigned char*)it->first.c_str();
		nv.namelen = it->first.size();
		nv.value = (unsigned char*)it->second.c_str();
		nv.valuelen = it->second.size();
		nv.flags = NGHTTP2_NV_FLAG_NONE;
		headers.push_back(nv);
	}

	nghttp2_data_provider data_prd;
	data_prd.read_callback = data_prd_read_callback;
	nghttp2_submit_request(h2_session, NULL, headers.data(),headers.size(), &data_prd, this);
	if(h2_session && !nghttp2_session_want_write(h2_session)){
		lyramilk::klog(lyramilk::log::warning,"mpush.aioapnspusher.notify_out") << lyramilk::kdict("HTTP2不可写") << std::endl;
		return false;
	}

	int r = nghttp2_session_send(h2_session);
	if(r<0){
		lyramilk::klog(lyramilk::log::warning,"mpush.aioapnspusher.notify_out") << lyramilk::kdict("HTTP2错误:%s",nghttp2_strerror(r)) << std::endl;
		return false;
	}
	return pool->reset(this,flag);
}

bool aioapnspusher::notify_hup()
{
	return false;
}

bool aioapnspusher::notify_err()
{
	return false;
}

bool aioapnspusher::notify_pri()
{
	TODO();
}

bool aioapnspusher::notify_attach(lyramilk::io::aiopoll* container)
{
	pool = container;
	return true;
}

bool aioapnspusher::notify_detach(lyramilk::io::aiopoll* container)
{
	aioapnspusher* p = new aioapnspusher();
	p->init_ssl(apns_cert,apns_cert_password);
	while(true){
		if(p->open(apns_host,apns_port)){
			container->add(p,EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLONESHOT);
			lyramilk::klog(lyramilk::log::trace,"mpush") << lyramilk::kdict("重新连接成功") << std::endl;
			return true;
		}else{
			lyramilk::klog(lyramilk::log::warning,"mpush") << lyramilk::kdict("重新连接%s:%d失败",apns_host.c_str(),apns_port) << std::endl;
		}
	}
	return true;
}

lyramilk::io::native_filedescriptor_type aioapnspusher::getfd()
{
	return sock;
}


//////////////////////////

int aioapnspusher::select_next_proto_cb(SSL *ssl, unsigned char **out,unsigned char *outlen, const unsigned char *in,unsigned int inlen, void *arg)
{
	int rv;
	rv = nghttp2_select_next_protocol(out, outlen, in, inlen);
	if (rv <= 0)
	{
		lyramilk::klog(lyramilk::log::warning,"mpush.select_next_proto_cb") << lyramilk::kdict("服务不支持HTTP2协议") << std::endl;
		exit(-1);
	}
	return SSL_TLSEXT_ERR_OK;
}

ssize_t aioapnspusher::send_callback(nghttp2_session *session, const uint8_t *data,size_t length, int flags, void *user_data)
{
	aioapnspusher* p = (aioapnspusher*)user_data;
	return SSL_write(p->sslobj,data,length);
}

ssize_t aioapnspusher::recv_callback(nghttp2_session *session, uint8_t *buf,size_t length, int flags, void *user_data)
{
	aioapnspusher* p = (aioapnspusher*)user_data;
	int r = SSL_read(p->sslobj,buf,length);
	if(r == -1){
		int err = SSL_get_error(p->sslobj, r);
		if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
			return NGHTTP2_ERR_WOULDBLOCK;
		}else{
			return NGHTTP2_ERR_CALLBACK_FAILURE;
		}
	}else if(r == 0){
		return NGHTTP2_ERR_EOF;
	}
	return r;
}

int aioapnspusher::on_frame_send_callback(nghttp2_session *session,const nghttp2_frame *frame,void *user_data)
{
	return 0;
}

int aioapnspusher::on_frame_recv_callback(nghttp2_session *session,const nghttp2_frame *frame,void *user_data)
{
	return 0;
}

int aioapnspusher::on_header_callback(nghttp2_session *session,const nghttp2_frame *frame,const uint8_t *name, size_t namelen,const uint8_t *value, size_t valuelen,uint8_t flags, void *user_data)
{
	if (frame->hd.type == NGHTTP2_HEADERS){
		aioapnspusher* p = (aioapnspusher*)user_data;
		p->responseheader[lyramilk::data::string((const char*)name, namelen)] = lyramilk::data::string((const char*)value, valuelen);
	}
	return 0;
}

int aioapnspusher::on_begin_headers_callback(nghttp2_session *session,const nghttp2_frame *frame,void *user_data)
{
	return 0;
}

int aioapnspusher::on_stream_close_callback(nghttp2_session *session, int32_t stream_id,uint32_t error_code,void *user_data)
{
	aioapnspusher* p = (aioapnspusher*)user_data;
	lyramilk::data::string apnsid;
	lyramilk::data::var& vapnsid = p->responseheader["apns-id"];
	if(vapnsid.type() == lyramilk::data::var::t_str){
		apnsid = vapnsid.str();
	}
	lyramilk::teapoy::redis::redis_client& redis = p->redis;

	if(!apnsid.empty()){
		if(p){
			int i = p->responseheader[":status"];
			if(i == 200){
				//成功
				lyramilk::klog(lyramilk::log::trace,"mpush.http2.on_stream_close") << "投递成功：" << p->responseheader << std::endl;
				{
					lyramilk::data::var ret;
					lyramilk::data::var::array args;
					args.push_back("hdel");
					args.push_back("mpush:id");
					args.push_back(apnsid);
					redis.exec(args,ret);
				}
			}else{
				lyramilk::klog(lyramilk::log::warning,"mpush.http2.on_stream_close") << "投递失败：" << p->responseheader << ",原因" << p->body << std::endl;

				lyramilk::data::var vbody;
				lyramilk::data::json::parse(p->body,&vbody);
				if(vbody.type() == lyramilk::data::var::t_map){
					lyramilk::data::string reason = vbody["reason"].str();
					if(reason == "TooManyRequests" || reason == "PayloadEmpty"){
						lyramilk::data::var ret;
						lyramilk::data::var::array args;
						args.push_back("hget");
						args.push_back("mpush:id");
						args.push_back(apnsid);
						redis.exec(args,ret);
						if(ret.type() == lyramilk::data::var::t_str){
							lyramilk::data::string taskstr = ret.str();

							lyramilk::data::var vtask;
							lyramilk::data::json::parse(taskstr,&vtask);
							vtask["apnsid"] = apnsid;

							lyramilk::data::string newtaskstr;
							lyramilk::data::json::stringify(vtask,&newtaskstr);

							{
								lyramilk::data::var ret;
								lyramilk::data::var::array args;
								args.push_back("hdel");
								args.push_back("mpush:id");
								args.push_back(apnsid);
								redis.exec(args,ret);
							}

							lyramilk::data::var ret;
							lyramilk::data::var::array args;
							args.push_back("rpush");
							args.push_back("mpush:queue");
							args.push_back(newtaskstr);
							if(redis.exec(args,ret)){
								p->responseheader.clear();
								p->body.clear();
								return 0;
							}
						}
					}
				}

				lyramilk::data::stringstream ss;
				ss << p->responseheader << "," << p->body;

				lyramilk::data::var ret;
				lyramilk::data::var::array args;
				args.push_back("hset");
				args.push_back("mpush:error");
				args.push_back(apnsid);
				args.push_back(ss.str());
				redis.exec(args,ret);
			}
		}else{
			lyramilk::klog(lyramilk::log::error,"mpush.http2.on_stream_close") << "投递失败：" << p->responseheader << ",原因" << p->body << std::endl;
		}
	}
	p->responseheader.clear();
	p->body.clear();
	return 0;
}

int aioapnspusher::on_data_chunk_recv_callback(nghttp2_session *session,uint8_t flags, int32_t stream_id,const uint8_t *data, size_t len,void *user_data)
{
	aioapnspusher* p = (aioapnspusher*)user_data;
	p->body.assign((const char*)data,len);
	return 0;
}

ssize_t aioapnspusher::data_prd_read_callback(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length,uint32_t *data_flags, nghttp2_data_source *source, void *user_data)
{
	aioapnspusher* p = (aioapnspusher*)user_data;
	p->payload.read((char*)buf,length);

	if(!p->payload.rdbuf()->in_avail()){
		*data_flags |= NGHTTP2_DATA_FLAG_EOF;
	}
	return p->payload.gcount();
}

