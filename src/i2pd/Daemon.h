#ifndef DAEMON_H__
#define DAEMON_H__

#include <memory>
#include <string>
#include <ostream>

namespace i2p
{
namespace util
{
	class Daemon_Singleton_Private;
	class Daemon_Singleton
	{
		public:
            virtual bool init(int argc, char* argv[], bool randomPorts, std::shared_ptr<std::ostream> logstream);
            virtual bool init(int argc, char* argv[], bool randomPorts);
            //virtual bool init(int argc, const char* const argv[], std::shared_ptr<std::ostream> logstream);
            //virtual bool init(int argc, const char* const argv[]);
            virtual bool start();
			virtual bool stop();
			virtual void run () {};

			bool isDaemon;
			bool running;

		protected:
			Daemon_Singleton();
			virtual ~Daemon_Singleton();

			bool IsService () const;

			// d-pointer for httpServer, httpProxy, etc.
			class Daemon_Singleton_Private;
			Daemon_Singleton_Private &d;
	};

#define Daemon i2p::util::DaemonLinux::Instance()
	class DaemonLinux : public Daemon_Singleton
	{
		public:
			static DaemonLinux& Instance()
			{
				static DaemonLinux instance;
				return instance;
			}

            bool start()
            {
                return Daemon_Singleton::start();
            }
            bool stop()
            {
                return Daemon_Singleton::stop();
            }
            void run()
            {
                Daemon_Singleton::run();
            }

	};
}
}

#endif // DAEMON_H__
