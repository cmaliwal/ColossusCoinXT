#ifndef I2PPURETUNNEL_H__
#define I2PPURETUNNEL_H__

#include <inttypes.h>
#include <string>
#include <set>
#include <tuple>
#include <memory>
#include <sstream>
#include <boost/asio.hpp>
#include "Identity.h"
#include "Destination.h"
#include "Datagram.h"
#include "Streaming.h"
#include "I2PService.h"

namespace i2p
{
    namespace client
    {
        //const size_t I2P_TUNNEL_CONNECTION_BUFFER_SIZE = 65536;
        //const int I2P_TUNNEL_CONNECTION_MAX_IDLE = 3600; // in seconds
        //const int I2P_TUNNEL_DESTINATION_REQUEST_TIMEOUT = 10; // in seconds
        //// for HTTP tunnels
        //const char X_I2P_DEST_HASH[] = "X-I2P-DestHash"; // hash  in base64
        //const char X_I2P_DEST_B64[] = "X-I2P-DestB64"; // full address in base64
        //const char X_I2P_DEST_B32[] = "X-I2P-DestB32"; // .b32.i2p address

        class I2PPureClientTunnel;
        class I2PPureTunnelConnection;
        class I2PPureServerTunnel;
        class I2PPureClientTunnelHandler;

        typedef std::function<void(std::shared_ptr<I2PPureClientTunnel> tunnel)> StreamCreatedCallback;
        typedef std::function<void(std::shared_ptr<I2PPureTunnelConnection> connection)> ConnectionCreatedCallback;
        typedef std::function<void(std::shared_ptr<I2PPureTunnelConnection> connection)> ClientConnectedCallback;
        typedef std::function<void(const boost::system::error_code& ecode)> ContinueToReceiveCallback;
        typedef std::function<void(std::string, ContinueToReceiveCallback)> ReceivedCallback;
        typedef std::function<void(const char* msg, size_t len)> SendCallback;
        typedef std::function<void()> ReadyToSendCallback;
        typedef std::function<void(const boost::system::error_code& ecode)> ErrorSendCallback;
        typedef std::function<void(const char*, size_t, ReadyToSendCallback, ErrorSendCallback)> SendMoreCallback;

        // this is really the same callback signature as the ClientConnected, but let's separate if needed
        typedef std::function<void(std::shared_ptr<I2PPureServerTunnel> tunnel)> ServerStreamAcceptedCallback;
        typedef std::function<void(std::shared_ptr<I2PPureServerTunnel> tunnel, std::shared_ptr<i2p::client::I2PPureTunnelConnection> connection)> ServerConnectionCreatedCallback;
        typedef std::function<void(std::shared_ptr<I2PPureServerTunnel> tunnel, std::shared_ptr<i2p::client::I2PPureTunnelConnection> connection)> ServerClientConnectedCallback;

        class I2PPureTunnelConnection : public I2PServiceHandler, public std::enable_shared_from_this<I2PPureTunnelConnection>
        {
        public:
            ClientConnectedCallback _connectedCallback;
            ReceivedCallback _receivedCallback;

        public:
            // std::shared_ptr<boost::asio::ip::tcp::socket> socket,
            I2PPureTunnelConnection(I2PService * owner, std::shared_ptr<const i2p::data::LeaseSet> leaseSet, int port = 0); // to I2P
            I2PPureTunnelConnection(I2PService * owner, std::shared_ptr<i2p::stream::Stream> stream,
                ClientConnectedCallback connectedCallback, ReceivedCallback receivedCallback); // to I2P using simplified API
            I2PPureTunnelConnection(I2PService * owner, std::shared_ptr<i2p::stream::Stream> stream, const boost::asio::ip::tcp::endpoint& target, bool quiet = true); // from I2P
            ~I2PPureTunnelConnection();
            void I2PConnect(const uint8_t * msg = nullptr, size_t len = 0);
            void Connect(bool isUniqueLocal = true);

            void HandleSendReady(std::string reply, ReadyToSendCallback readyToSend, ErrorSendCallback errorSend);
            void HandleSendReadyRaw(const uint8_t * buf, size_t len, ReadyToSendCallback readyToSend, ErrorSendCallback errorSend);
            void HandleSendReadyRawSigned(const char* buf, size_t len, ReadyToSendCallback readyToSend, ErrorSendCallback errorSend);
            void HandleSend(std::string reply);
            void HandleSendRaw(const uint8_t * buf, size_t len);
            void HandleSendRawSigned(const char* buf, size_t len);

            void HandleWriteAsync(const boost::system::error_code& ecode);
            // moved to public to be able to callback
            //void HandleWrite(const boost::system::error_code& ecode);

        protected:
            void Terminate();

            void Receive();
            void HandleReceived(const boost::system::error_code& ecode, std::size_t bytes_transferred);
            virtual void Write(const uint8_t * buf, size_t len); // can be overloaded
            void HandleWrite(const boost::system::error_code& ecode);

            void StreamReceive();
            void HandleStreamReceive(const boost::system::error_code& ecode, std::size_t bytes_transferred);
            void HandleConnect(const boost::system::error_code& ecode);

            //std::shared_ptr<const boost::asio::ip::tcp::socket> GetSocket() const { return m_Socket; };

        private:
            uint8_t m_Buffer[I2P_TUNNEL_CONNECTION_BUFFER_SIZE], m_StreamBuffer[I2P_TUNNEL_CONNECTION_BUFFER_SIZE];
            //std::shared_ptr<boost::asio::ip::tcp::socket> m_Socket;
            std::shared_ptr<i2p::stream::Stream> m_Stream;
            //boost::asio::ip::tcp::endpoint m_RemoteEndpoint;
            bool m_IsQuiet; // don't send destination
        };

        // test
        class I2PPureTest 
        {
            I2PPureTest() {}
        };

        class I2PPureClientTunnel : public TCPIPAcceptor
        {
            StreamCreatedCallback _streamCreated;
            ConnectionCreatedCallback _connectionCreatedCallback;
            ClientConnectedCallback _connectedCallback;
            ReceivedCallback _receivedCallback;
            SendCallback _sendCallback;
            SendMoreCallback _sendMoreCallback;
            // fast fix, use handlers set to find it (or plural?)
            //std::shared_ptr<I2PPureClientTunnelHandler> _myHandler;

        protected:
            // Implements TCPIPAcceptor
            std::shared_ptr<I2PServiceHandler> CreateHandler(std::shared_ptr<boost::asio::ip::tcp::socket> socket);
            //std::shared_ptr<I2PServiceHandler> CreateHandler(
            //    std::shared_ptr<boost::asio::ip::tcp::socket> socket,
            //    ClientConnectedCallback clientConnected);

        public:
            I2PPureClientTunnel(const std::string& name, const std::string& destination,
                const std::string& address, int port, std::shared_ptr<ClientDestination> localDestination, int destinationPort = 0, StreamCreatedCallback streamCreated = nullptr); // , ClientConnectedCallback clientConnected = nullptr);
            ~I2PPureClientTunnel() {}

            // callbacks have to be set a bit later on (ctor goes before node is created).

            void SetStreamCreatedCallback(StreamCreatedCallback streamCreated) { _streamCreated = streamCreated; }
            StreamCreatedCallback GetStreamCreatedCallback() { return _streamCreated; }

            void SetConnectionCreatedCallback(ConnectionCreatedCallback connectionCreatedCallback) { _connectionCreatedCallback = connectionCreatedCallback; }
            ConnectionCreatedCallback GetConnectionCreatedCallback() { return _connectionCreatedCallback; }

            void SetConnectedCallback(ClientConnectedCallback connectedCallback) { _connectedCallback = connectedCallback; }
            ClientConnectedCallback GetConnectedCallback() { return _connectedCallback; }

            void SetReceivedCallback(ReceivedCallback receivedCallback) { _receivedCallback = receivedCallback; }
            ReceivedCallback GetReceivedCallback() { return _receivedCallback; }
            
            void SetSendCallback(SendCallback sendCallback) { _sendCallback = sendCallback; }
            SendCallback GetSendCallback() { return _sendCallback; }

            void SetSendMoreCallback(SendMoreCallback sendMoreCallback) { _sendMoreCallback = sendMoreCallback; }
            SendMoreCallback GetSendMoreCallback() { return _sendMoreCallback; }

            void Start();
            void Stop();

            const char* GetName() { return m_Name.c_str(); }
            const char* GetDestination() { return m_Destination.c_str(); }

        private:
            const i2p::data::IdentHash * GetIdentHash();

        private:
            std::string m_Name, m_Destination;
            const i2p::data::IdentHash * m_DestinationIdentHash;
            int m_DestinationPort;
        };

        class I2PPureServerTunnel : public I2PService
        {
            ServerStreamAcceptedCallback _acceptedCallback;
            ServerConnectionCreatedCallback _connectionCreatedCallback;
            ServerClientConnectedCallback _clientConnectedCallback;
            ReceivedCallback _receivedCallback;
            SendCallback _sendCallback;
            SendMoreCallback _sendMoreCallback;

        public:
            I2PPureServerTunnel(const std::string& name, const std::string& address, int port,
                std::shared_ptr<ClientDestination> localDestination, int inport = 0, bool gzip = true,
                ServerStreamAcceptedCallback acceptedCallback = nullptr);

            // callbacks have to be set a bit later on (ctor goes before node is created).
            void SetAcceptedCallback(ServerStreamAcceptedCallback acceptedCallback) { _acceptedCallback = acceptedCallback; }
            ServerStreamAcceptedCallback GetAcceptedCallback() { return _acceptedCallback; }

            void SetConnectionCreatedCallback(ServerConnectionCreatedCallback connectionCreatedCallback) { _connectionCreatedCallback = connectionCreatedCallback; }
            ServerConnectionCreatedCallback GetConnectionCreatedCallback() { return _connectionCreatedCallback; }

            void SetConnectedCallback(ServerClientConnectedCallback connectedCallback) { _clientConnectedCallback = connectedCallback; }
            ServerClientConnectedCallback GetConnectedCallback() { return _clientConnectedCallback; }

            void SetReceivedCallback(ReceivedCallback receivedCallback) { _receivedCallback = receivedCallback; }
            ReceivedCallback GetReceivedCallback() { return _receivedCallback; }

            void SetSendCallback(SendCallback sendCallback) { _sendCallback = sendCallback; }
            SendCallback GetSendCallback() { return _sendCallback; }

            void SetSendMoreCallback(SendMoreCallback sendMoreCallback) { _sendMoreCallback = sendMoreCallback; }
            SendMoreCallback GetSendMoreCallback() { return _sendMoreCallback; }


            void Start();
            void Stop();

            void SetAccessList(const std::set<i2p::data::IdentHash>& accessList);

            void SetUniqueLocal(bool isUniqueLocal) { m_IsUniqueLocal = isUniqueLocal; }
            bool IsUniqueLocal() const { return m_IsUniqueLocal; }

            const std::string& GetAddress() const { return m_Address; }
            int GetPort() const { return m_Port; };
            uint16_t GetLocalPort() const { return m_PortDestination->GetLocalPort(); };
            const boost::asio::ip::tcp::endpoint& GetEndpoint() const { return m_Endpoint; }

            const char* GetName() { return m_Name.c_str(); }

        private:
            void HandleResolve(const boost::system::error_code& ecode, boost::asio::ip::tcp::resolver::iterator it,
                std::shared_ptr<boost::asio::ip::tcp::resolver> resolver);

            void Accept();
            void HandleAccept(std::shared_ptr<i2p::stream::Stream> stream);
            virtual std::shared_ptr<I2PPureTunnelConnection> CreateI2PConnection(std::shared_ptr<i2p::stream::Stream> stream);

        private:
            bool m_IsUniqueLocal;
            std::string m_Name, m_Address;
            int m_Port;
            boost::asio::ip::tcp::endpoint m_Endpoint;
            std::shared_ptr<i2p::stream::StreamingDestination> m_PortDestination;
            std::set<i2p::data::IdentHash> m_AccessList;
            bool m_IsAccessList;
        };
    }
}

#endif
