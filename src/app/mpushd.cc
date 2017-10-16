#include <getopt.h>
#include <libmilk/log.h>
#include <libmilk/multilanguage.h>
#include <sys/epoll.h>
#include <sys/wait.h>

#include "config.h"
#include "guid.h"
#include "push_worker.h"

static lyramilk::data::string get_topic (const lyramilk::data::string& path)
{
	BIO  *bio = NULL;
	if (NULL == (bio = BIO_new_file(path.c_str(), "r"))){
		lyramilk::klog(lyramilk::log::warning,"mpush") << lyramilk::kdict("读取证书时出现错误1") << std::endl;
		return "";
	}
	X509 *x509 = PEM_read_bio_X509_AUX(bio, NULL, NULL, NULL);
	BIO_free(bio);

	X509_NAME *xn = X509_get_subject_name(x509);
	int cnt = X509_NAME_entry_count(xn);
	int pos = X509_NAME_get_index_by_NID(xn, NID_userId, -1);
	if (pos >= 0 && pos <= cnt){
		ASN1_STRING *d = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(xn, pos));
		lyramilk::data::string ret((char*)d->data,d->length);
		X509_free(x509);
		return ret;
	}
	X509_free(x509);
	lyramilk::klog(lyramilk::log::warning,"mpush") << lyramilk::kdict("读取证书时出现错误2") << std::endl;
	return "";
}

class teapoy_log_logfile:public lyramilk::log::logb
{
	mutable FILE* fp;
	lyramilk::data::string logfilepath;
	lyramilk::data::string pidstr;
	lyramilk::data::string str_debug;
	lyramilk::data::string str_trace;
	lyramilk::data::string str_warning;
	lyramilk::data::string str_error;
	mutable lyramilk::threading::mutex_os lock;
	mutable tm daytime;
  public:
	teapoy_log_logfile(lyramilk::data::string logfilepath)
	{
		this->logfilepath = logfilepath;
		fp = fopen(logfilepath.c_str(),"a");
		if(!fp) fp = stdout;
		pid_t pid = getpid();
		lyramilk::data::stringstream ss;
		ss << pid << " ";
		pidstr = ss.str();
		str_debug = "[" + D("debug") + "] ";
		str_trace = "[" + D("trace") + "] ";
		str_warning = "[" + D("warning") + "] ";
		str_error = "[" + D("error") + "] ";

		time_t stime = time(0);
		daytime = *localtime(&stime);
	}
	virtual ~teapoy_log_logfile()
	{
		if(fp != stdout) fclose(fp);
	}

	virtual bool ok()
	{
		return fp != nullptr;
	}

	virtual void log(time_t ti,lyramilk::log::type ty,lyramilk::data::string usr,lyramilk::data::string app,lyramilk::data::string module,lyramilk::data::string str) const
	{
		tm t;
		localtime_r(&ti,&t);
		if(daytime.tm_year != t.tm_year || daytime.tm_mon != t.tm_mon || daytime.tm_mday != t.tm_mday){
			lyramilk::threading::mutex_sync _(lock);
			if(daytime.tm_year != t.tm_year || daytime.tm_mon != t.tm_mon || daytime.tm_mday != t.tm_mday){
				char buff[64];
				snprintf(buff,sizeof(buff),".%04d%02d%02d",(1900 + daytime.tm_year),(daytime.tm_mon + 1),daytime.tm_mday);
				daytime = t;
				lyramilk::data::string destfilename = logfilepath;
				destfilename.append(buff);
				rename(logfilepath.c_str(),destfilename.c_str());
				FILE* newfp = fopen(logfilepath.c_str(),"a");
				if(newfp){
					FILE* oldfp = fp;
					fp = newfp;
					if(oldfp && oldfp != stdout) fclose(oldfp);
				}
			}
		}
		lyramilk::data::string cache;
		cache.reserve(1024);
		switch(ty){
		  case lyramilk::log::debug:
			cache.append(pidstr);
			cache.append(str_debug);
			cache.append(strtime(ti));
			cache.append(" [");
			cache.append(module);
			cache.append("] ");
			cache.append(str);
			fwrite(cache.c_str(),cache.size(),1,fp);
			fflush(fp);
			break;
		  case lyramilk::log::trace:
			cache.append(pidstr);
			cache.append(str_trace);
			cache.append(strtime(ti));
			cache.append(" [");
			cache.append(module);
			cache.append("] ");
			cache.append(str);
			fwrite(cache.c_str(),cache.size(),1,fp);
			fflush(fp);
			break;
		  case lyramilk::log::warning:
			cache.append(pidstr);
			cache.append(str_warning);
			cache.append(strtime(ti));
			cache.append(" [");
			cache.append(module);
			cache.append("] ");
			cache.append(str);
			fwrite(cache.c_str(),cache.size(),1,fp);
			fflush(fp);
			break;
		  case lyramilk::log::error:
			cache.append(pidstr);
			cache.append(str_error);
			cache.append(strtime(ti));
			cache.append(" [");
			cache.append(module);
			cache.append("] ");
			cache.append(str);
			fwrite(cache.c_str(),cache.size(),1,fp);
			fflush(fp);
			break;
		}
	}
};


class mypoll:public lyramilk::io::aiopoll
{
	virtual void onevent(lyramilk::io::aioselector* r,lyramilk::io::uint32 events)
	{
		if(events & EPOLLIN){
			events &= ~EPOLLOUT;
		}
		return lyramilk::io::aiopoll::onevent(r,events);
	}
};

#define gettext
void useage(lyramilk::data::string selfname)
{
	std::cout << gettext("useage:") << selfname << " [optional]" << std::endl;
	std::cout << "\t-d --daemon                                  \t" << gettext("以服务方式启动") << std::endl;
	std::cout << "\t-t --thread             <线程数>             \t" << gettext("程序启动的线程数目：默认") << threadcount << std::endl;
	std::cout << "\t-c --concurrency        <并发客户端数>       \t" << gettext("并发客户端数目：默认") << concurrency << std::endl;
	std::cout << "\t-h --redis-host         <redis Host>         \t" << gettext("缓存redis的host：默认") << redis_host << std::endl;
	std::cout << "\t-p --redis-port         <redis端口>          \t" << gettext("缓存redis的port：默认") << redis_port << std::endl;
	std::cout << "\t-a --redis-password     <redis密码>          \t" << gettext("缓存redis的密码：默认无密码") << std::endl;
	std::cout << "\t-n --apns-host          <苹果服务器Host>     \t" << gettext("苹果推送服务器Host：默认") << apns_host << std::endl;
	std::cout << "\t-s --apns-port          <苹果服务器Port>     \t" << gettext("苹果推送服务器Port：默认") << apns_port << std::endl;
	std::cout << "\t-i --apns-topic         <应用topic>          \t" << gettext("应用topic：默认从证书中获取") << apns_topic << std::endl;
	std::cout << "\t-r --apns-cert          <推送用的SSL证书>    \t" << gettext("苹果推送服务证书：默认") << apns_cert << std::endl;
	std::cout << "\t-w --apns-cert-password <推送用的SSL证书密码>\t" << gettext("苹果推送服务证书密码：默认******") << std::endl;
	std::cout << "\t-l --log-file           <日志文件>           \t" << gettext("日志文件：默认不使用日志") << std::endl;
	std::cout << "\t-? --help                                    \t显示这个帮助" << std::endl;
	std::cout << "在缓存redis中，向List(key=mpush:queue)存入数据K，本程序就会将其发送出去" << std::endl;
	std::cout << "K是一个json，其中包含要发送到的token和实际给苹果的json的字符串形式" << std::endl;
	std::cout << "K例子：{\"token\":\"ad2b5cbaa1027495d4e0274a95259318a4f89d284696acb38b6825ef92f5c2a1\",\"data\":\"{\\\"aps\\\":{\\\"alert\\\":\\\"apn test.1 2017-05-02 11:46:29\\\",\\\"sound\\\":\\\"default\\\"}}\"}" << std::endl;
}

struct option long_options[] = {
	{ "thread", required_argument, NULL, 't' },
	{ "concurrency", required_argument, NULL, 'c' },
	{ "redis-host", required_argument, NULL, 'h' },
	{ "redis-port", required_argument, NULL, 'p' },
	{ "redis-password", required_argument, NULL, 'a' },
	{ "apns-host", required_argument, NULL, 'n' },
	{ "apns-port", required_argument, NULL, 's' },
	{ "apns-cert", required_argument, NULL, 'r' },
	{ "apns-cert-password", required_argument, NULL, 'w' },
	{ "log-file", required_argument, NULL, 'l' },
	{ "help", no_argument, NULL, '?' },
	{ 0, 0, 0, 0},
};

bool isdaemon = false;
bool ondaemon = false;
int main(int argc,char* argv[])
{
	lyramilk::data::string selfname = argv[0];
	if(argc > 1){
		int oc;
		while((oc = getopt_long(argc, argv, "dt:c:h:p:a:n:s:r:w:l:?",long_options,nullptr)) != -1){
			switch(oc)
			{
			  case 'd':
				isdaemon = true;
				break;
			  case 't':
				threadcount = atoi(optarg);
				break;
			  case 'c':
				concurrency = atoi(optarg);
				break;
			  case 'h':
				redis_host = optarg;
				break;
			  case 'p':
				redis_port = atoi(optarg);
				break;
			  case 'a':
				redis_password = optarg;
				break;
			  case 'n':
				apns_host = optarg;
				break;
			  case 's':
				apns_port = atoi(optarg);
				break;
			  case 'r':
				apns_cert = optarg;
				break;
			  case 'w':
				apns_cert_password = optarg;
				break;
			  case 'l':
				log_file = optarg;
				break;
			  case '?':
			  default:
				useage(selfname);
				return 0;
			}
		}
	}else{
		useage(selfname);
		return 0;
	}

	std::cout << "线程数  \t" << threadcount << std::endl;
	std::cout << "并发数  \t" << concurrency << std::endl;
	std::cout << "Redis   \t" << redis_host << ":" << redis_port << "\t" << redis_password << std::endl;
	std::cout << "APNs设置\t" << apns_host << ":" << apns_port << std::endl;
	std::cout << "APNs证书\t" << apns_cert << std::endl;
	if(apns_topic.empty()){
		apns_topic = get_topic(apns_cert);
	}
	std::cout << "APNs主题\t" << apns_topic << std::endl;

	if(isdaemon){
		::daemon(1,1);
		int pid = 0;
		do{
			pid = fork();
			if(pid == 0){
				ondaemon = true;
				break;
			}
		}while(waitpid(pid,NULL,0));
	}

	if(!log_file.empty()){
		std::cout << "日志    \t" << log_file << std::endl;
		lyramilk::klog.rebase(new teapoy_log_logfile(log_file));
	}

	//signal(SIGPIPE, SIG_IGN);

	mypoll pool;
	pool.active(threadcount);
	/**/
	for(int i=0;i<concurrency;++i){
		aioapnspusher* p = new aioapnspusher();
		p->init_ssl(apns_cert,apns_cert_password);
		if(p->open(apns_host,apns_port)){
			pool.add(p,EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLONESHOT);
			lyramilk::klog(lyramilk::log::trace,"mpush") << lyramilk::kdict("连接成功") << std::endl;
		}else{
			lyramilk::klog(lyramilk::log::warning,"mpush") << lyramilk::kdict("连接失败") << std::endl;
			--i;
		}
	}
	while(true){
		sleep(1);
	}
	return 0;
}

/*
{
	aioapnspusher* p = new aioapnspusher();
	p->init_ssl("/usr/local/src/push/key/ck_development.pem","yeelion");
	if(p->open("api.development.push.apple.com",443)){
		pool.add(p,EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLONESHOT);
	}else{
		return -1;
	}
}
*/
