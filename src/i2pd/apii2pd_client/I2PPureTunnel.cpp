#define BOOST_BIND_NO_PLACEHOLDERS

#include <cassert>
#include "Base.h"
#include "Log.h"
#include "Destination.h"
#include "ClientContext.h"
#include "I2PPureTunnel.h"
#include "util.h"

namespace i2p
{
    namespace client
    {

        ///** set standard socket options */
        //static void I2PPureTunnelSetSocketOptions(std::shared_ptr<boost::asio::ip::tcp::socket> socket)
        //{
        //	if (socket && socket->is_open())
        //	{
        //		boost::asio::socket_base::receive_buffer_size option(I2P_TUNNEL_CONNECTION_BUFFER_SIZE);
        //		socket->set_option(option);
        //	}
        //}

        //std::shared_ptr<boost::asio::ip::tcp::socket> socket,
        I2PPureTunnelConnection::I2PPureTunnelConnection(
            I2PService * owner, std::shared_ptr<const i2p::data::LeaseSet> leaseSet, int port) :
            I2PServiceHandler(owner), 
            //m_Socket(socket), 
            //m_RemoteEndpoint(socket->remote_endpoint()),
            m_IsQuiet(true),
            _hasTimedOut(false),
            _timedOutTime(0)
        {
            m_Stream = GetOwner()->GetLocalDestination()->CreateStream(leaseSet, port);
        }

        I2PPureTunnelConnection::I2PPureTunnelConnection(
            I2PService * owner, 
            std::shared_ptr<i2p::stream::Stream> stream,
            ReceivedCallback receivedCallback,
            ConnectionTimedOutCallback timedOutCallback) : // ClientConnectedCallback connectedCallback, 
            I2PServiceHandler(owner), 
            //m_Socket(socket), 
            m_Stream(stream),
            //_connectedCallback(connectedCallback),
            _receivedCallback(receivedCallback),
            _timedOutCallback(timedOutCallback),
            //m_RemoteEndpoint(socket->remote_endpoint()), 
            m_IsQuiet(true),
            _hasTimedOut(false),
            _timedOutTime(0)
        {
        }

        I2PPureTunnelConnection::I2PPureTunnelConnection(
            I2PService * owner, 
            std::shared_ptr<i2p::stream::Stream> stream,
            //std::shared_ptr<boost::asio::ip::tcp::socket> socket, 
            const boost::asio::ip::tcp::endpoint& target, 
            ReceivedCallback receivedCallback,
            ConnectionTimedOutCallback timedOutCallback,
            bool quiet) :
            I2PServiceHandler(owner), 
            //m_Socket(socket), 
            m_Stream(stream),
            _receivedCallback(receivedCallback),
            _timedOutCallback(timedOutCallback),
            //m_RemoteEndpoint(target), 
            m_IsQuiet(quiet),
            _hasTimedOut(false),
            _timedOutTime(0)
        {
        }

        I2PPureTunnelConnection::~I2PPureTunnelConnection()
        {
        }

        // this is the client tunnel variant (only client calls this, server does it via Connect/Handle
        void I2PPureTunnelConnection::I2PConnect(const uint8_t * msg, size_t len)
        {
            if (m_Stream)
            {
                if (msg)
                    m_Stream->Send(msg, len); // connect and send
                else {
                    // if we'd like to empty buffer in case something was posted for send before connect we'd need
                    // a better buffer, i.e. bytes_transferred saved alongside as well.
                    m_Stream->Send(m_Buffer, 0); // connect
                }
            }
            // this is from I2P network, anything arriving for us (inbound)
            StreamReceive();
            // this is wrongly named 'receive' due to the previous local socket mechanism, it's 'send'.
            Receive();
        }

        // this is server tunnel connect
        void I2PPureTunnelConnection::Connect(bool isUniqueLocal)
        {
            HandleConnect(boost::system::error_code());
        }

        void I2PPureTunnelConnection::Terminate()
        {
            if (Kill()) return;
            if (m_Stream)
            {
                m_Stream->Close();
                m_Stream.reset();
            }

            Done(shared_from_this());
        }

        void I2PPureTunnelConnection::Receive()
        {
            if (m_Stream){
                LogPrint(eLogDebug, "I2PPureTunnelConnection.Receive: Address ", m_Stream->GetRemoteIdentity()->GetIdentHash().ToBase32(), " - receiving...");
            }

            // this isn't doing anything for the moment, we need some event/signal to be fired when we place some
            // data for send - which would call HandleReceived
            // or we need a mutex here and wait on it to signal when new data arrived in the buffer to send.
            // this was previously a local socket async read wait calling HandleReceived
        }

        bool I2PPureTunnelConnection::HandleSendReady(std::string reply, ReadyToSendCallback readyToSend, ErrorSendCallback errorSend)
        {
            return HandleSendReadyRaw((const uint8_t *)reply.data(), reply.length(), readyToSend, errorSend);
        }

        bool I2PPureTunnelConnection::HandleSendReadyRawSigned(const char* buf, size_t len, ReadyToSendCallback readyToSend, ErrorSendCallback errorSend)
        {
            return HandleSendReadyRaw((const uint8_t *)buf, len, readyToSend, errorSend);
        }

        bool I2PPureTunnelConnection::HandleSendReadyRaw(const uint8_t * buf, size_t len, ReadyToSendCallback readyToSend, ErrorSendCallback errorSend)
        {
            if (m_Stream)
            {
                // message is never nullstr, and it shouldn't be empty() either
                std::string message((const char*)buf, std::min((int)len, 30)); // limit it just for logs
                LogPrint(eLogDebug, "I2PPureTunnelConnection.HandleSendReadyRaw: Sending to Address ", m_Stream->GetRemoteIdentity()->GetIdentHash().ToBase32(), " (outbound)...");
                LogPrint(eLogDebug, "I2PPureTunnelConnection.HandleSendReadyRaw: Sending... '", message, "' (", len, ")...");

                auto s = shared_from_this();
                // don't use strings, use buffers directly, less overhead plus safer w/ any encoding issues etc.
                //m_Stream->AsyncSend((const uint8_t *)reply.data(), reply.length(),
                m_Stream->AsyncSend(buf, len,
                    [s, readyToSend, errorSend](const boost::system::error_code& ecode)
                {
                    if (!ecode) {
                        if (readyToSend)
                            readyToSend();
                        // this is off because this->Receive above doesn't really do anything any more, should be removed
                        //s->Receive();
                    }
                    else {
                        if (errorSend)
                            errorSend(ecode);
                        s->Terminate();
                    }
                });
                
                return true;
            }
            return false;
        }

        // void I2PPureTunnelConnection::HandleSend(std::string reply)
        // {
        //     HandleSendRaw((const uint8_t *)reply.data(), reply.length());
        // }

        // void I2PPureTunnelConnection::HandleSendRawSigned(const char* buf, size_t len)
        // {
        //     HandleSendRaw((const uint8_t *)buf, len);
        // }
        // void I2PPureTunnelConnection::HandleSendRaw(const uint8_t * buf, size_t len)
        // {
        //     if (m_Stream)
        //     {
        //         std::string message((const char*)buf, std::min((int)len, 30));
        //         LogPrint(eLogDebug, "I2PPureTunnelConnection.HandleSendRaw: Sending to Address ", m_Stream->GetRemoteIdentity()->GetIdentHash().ToBase32(), " (outbound)...");
        //         LogPrint(eLogDebug, "I2PPureTunnelConnection.HandleSendRaw: Sending... '", message, "' (", len, ")...");

        //         LogPrint(eLogDebug, "I2PPureTunnelConnection.HandleSend: Sending to Address ", m_Stream->GetRemoteIdentity()->GetIdentHash().ToBase32(), " (outbound)...");

        //         auto s = shared_from_this();
                
        //         // Send is again AsyncSend, there's no such thing as 'send right now', w/ async we get the callback
        //         //m_Stream->Send((const uint8_t *)reply.data(), reply.length());

        //         //m_Stream->AsyncSend((const uint8_t *)reply.data(), reply.length(),
        //         m_Stream->AsyncSend(buf, len,
        //             [s](const boost::system::error_code& ecode)
        //         {
        //             if (!ecode)
        //                 s->Receive();
        //             else
        //                 s->Terminate();
        //         });
        //     }
        // }

        // This isn't really used any more, like Receive, should be removed, naming is also misleading left from local sockets.
        // this is now actually HandleSendReadyRaw()
        void I2PPureTunnelConnection::HandleReceived(const boost::system::error_code& ecode, std::size_t bytes_transferred)
        {
            if (ecode)
            {
                if (ecode != boost::asio::error::operation_aborted)
                {
                    LogPrint(eLogError, "I2PPureTunnelConnection: read error: ", ecode.message());
                    Terminate();
                }
            } else
            {
                if (m_Stream)
                {
                    std::string message((const char*)m_Buffer, std::min((int)bytes_transferred, 30));
                    LogPrint(eLogDebug, "I2PPureTunnelConnection.HandleReceived: Address ", m_Stream->GetRemoteIdentity()->GetIdentHash().ToBase32(), " - receiving...");
                    LogPrint(eLogDebug, "I2PPureTunnelConnection.HandleReceived: Sending... '", message, "' (", bytes_transferred, ")...");

                    auto s = shared_from_this();
                    m_Stream->AsyncSend(m_Buffer, bytes_transferred,
                        [s](const boost::system::error_code& ecode)
                    {
                        if (!ecode)
                            s->Receive();
                        else
                            s->Terminate();
                    });
                }
            }
        }

        // probably not needed but just to be safe when called from another thread (like node processing)
        void I2PPureTunnelConnection::HandleWriteAsync(const boost::system::error_code& ecode)
        {
            //auto service = GetOwner()->GetService();
            auto s = shared_from_this();
            GetOwner()->GetService().post([s, ecode](void)
            {
                s->HandleWrite(ecode);
            });
        }

        void I2PPureTunnelConnection::HandleWrite(const boost::system::error_code& ecode)
        {
            if (ecode)
            {
                LogPrint(eLogError, "I2PPureTunnelConnection: write error: ", ecode.message());
                if (ecode != boost::asio::error::operation_aborted)
                    Terminate();
            } else
                StreamReceive();
        }

        // loop around the I2P net stream, calls HandleStreamReceive -> Write -> HandleWrite -> back here.
        void I2PPureTunnelConnection::StreamReceive()
        {
            if (m_Stream)
            {
                LogPrint(eLogDebug, "I2PPureTunnelConnection.StreamReceive: Address ", m_Stream->GetRemoteIdentity()->GetIdentHash().ToBase32(), " - receiving from stream...");

                if (m_Stream->GetStatus() == i2p::stream::eStreamStatusNew ||
                    m_Stream->GetStatus() == i2p::stream::eStreamStatusOpen) // regular
                {
                    m_Stream->AsyncReceive(boost::asio::buffer(m_StreamBuffer, I2P_TUNNEL_CONNECTION_BUFFER_SIZE),
                        std::bind(&I2PPureTunnelConnection::HandleStreamReceive, shared_from_this(),
                            std::placeholders::_1, std::placeholders::_2),
                        I2P_TUNNEL_CONNECTION_MAX_IDLE);
                } else // closed by peer
                {
                    // get remaning data
                    auto len = m_Stream->ReadSome(m_StreamBuffer, I2P_TUNNEL_CONNECTION_BUFFER_SIZE);
                    if (len > 0) // still some data
                        Write(m_StreamBuffer, len);
                    else // no more data
                        Terminate();
                }
            }
        }

        // this is what gets called on a new I2P message over net
        void I2PPureTunnelConnection::HandleStreamReceive(const boost::system::error_code& ecode, std::size_t bytes_transferred)
        {
            if (m_Stream){
                LogPrint(eLogDebug, "I2PPureTunnelConnection.HandleStreamReceive: Address ", m_Stream->GetRemoteIdentity()->GetIdentHash().ToBase32(), " - received from stream...");
            }

            if (ecode)
            {
                if (ecode != boost::asio::error::operation_aborted)
                {
                    LogPrint(eLogError, "I2PPureTunnelConnection: stream read error: ", ecode.message());
                    if (bytes_transferred > 0)
                        Write(m_StreamBuffer, bytes_transferred); // postpone termination
                    else if (ecode == boost::asio::error::timed_out && m_Stream && m_Stream->IsOpen()) {
                        if (!_hasTimedOut)
                            _timedOutTime = i2p::util::time::GetTimeMillis();
                        _hasTimedOut = true;

                        if (_shouldTerminate) {
                            LogPrint(eLogError, "I2PPureTunnelConnection.HandleStreamReceive: node terminated... ");
                            Terminate();
                            return;
                        }
                        if (_timedOutCallback) {
                            _timedOutCallback(shared_from_this());
                            // LogPrint(eLogError, "I2PPureTunnelConnection: error: no received callback specified... ");
                        }
                        // don't keep timed out connections, just terminate & the node above will do the same (_timedOutCallback)
                        Terminate();
                        // StreamReceive();
                    }
                    else
                        Terminate();
                } else
                    Terminate();
            } else
                Write(m_StreamBuffer, bytes_transferred);
        }

        // we've got a new message...but we also spawn a wait for another async message receive.
        // the actual 'write' to a local socket has been removed (as a socket itself), so we loop back right away.
        void I2PPureTunnelConnection::Write(const uint8_t * buf, size_t len)
        {
            if (m_Stream){
                // message is never nullstr, and it shouldn't be empty() either
                std::string message((const char*)buf, std::min((int)len, 30));
                LogPrint(eLogDebug, "I2PPureTunnelConnection.Write: Address ", m_Stream->GetRemoteIdentity()->GetIdentHash().ToBase32(), " - received from stream...: ", message);
            }

            if (_shouldTerminate) {
                LogPrint(eLogError, "I2PPureTunnelConnection.Write: node terminated, terminating connection as well... ");
                Terminate();
                return;
            }
            if (!_receivedCallback) {
                LogPrint(eLogError, "I2PPureTunnelConnection: error: no received callback specified... ");
                Terminate();
                return;
            }

            _receivedCallback(buf, len); //, nullptr);

            if (_hasTimedOut) {
                _reactivated = true;
                _reactivatedTime = i2p::util::time::GetTimeMillis();
            }

            // TODO: we might want to add a callback to call back in here again (and continue the loop), when the received is processed
            // Note: this is the old way, we've now reorganized, we call from the Node when node has read the message and is ready
            // to continue receiving more data. I'm leaving this here as not sure if that's ideal.

            // change: this stops here after we send the message to the receiver, and awaiting receiver to call the
            // HandleWriteAsync in order to continue this 'loop'
        }

        void I2PPureTunnelConnection::HandleConnect(const boost::system::error_code& ecode)
        {
            if (m_Stream){
                LogPrint(eLogDebug, "I2PPureTunnelConnection.HandleConnect: connected.");
                LogPrint(eLogDebug, "I2PPureTunnelConnection.HandleConnect: Address ", i2p::client::context.GetAddressBook().ToAddress(m_Stream->GetRemoteIdentity()->GetIdentHash()), " - connected.");
            }

            if (ecode)
            {
                LogPrint(eLogError, "I2PPureTunnelConnection: connect error: ", ecode.message());
                Terminate();
            } else
            {
                LogPrint(eLogDebug, "I2PPureTunnelConnection: connected");
                if (m_IsQuiet)
                    StreamReceive();
                else
                {
                    StreamReceive();
                    // Note: this was the old way of 'not quiet' and sending local sockets some info about us, but that's 
                    // wracking our entire messages structure now (as Node et al don't expect anything like that).
                    // Also turn quiet on for all, server connection as well. This should go, leaving it till sure.

                    // // send destination first as if received from I2P
                    // std::string dest = m_Stream->GetRemoteIdentity()->ToBase64();
                    // dest += "\n";
                    // if (sizeof(m_StreamBuffer) >= dest.size()) {
                    //     memcpy(m_StreamBuffer, dest.c_str(), dest.size());
                    // }
                    // // shouldn't this go inside the 'if' above? this is a bug (but buffer is empty at this point so nothing usually happens)
                    // HandleStreamReceive(boost::system::error_code(), dest.size());
                }
                Receive();
            }
        }

        /* This handler tries to stablish a connection with the desired server and dies if it fails to do so */
        class I2PPureClientTunnelHandler : public I2PServiceHandler, public std::enable_shared_from_this<I2PPureClientTunnelHandler>
        {
            ClientConnectedCallback _connectedCallback;
            ReceivedCallback _receivedCallback;
            SendCallback _sendCallback;
            SendMoreCallback _sendMoreCallback;
            std::shared_ptr<I2PPureClientTunnel> _parentTunnel;

        public:
            I2PPureClientTunnelHandler(
                std::shared_ptr<I2PPureClientTunnel> parent,
                //I2PPureClientTunnel * parent, 
                i2p::data::IdentHash destination,
                int destinationPort, 
                std::shared_ptr<boost::asio::ip::tcp::socket> socket,
                ClientConnectedCallback connectedCallback = nullptr,
                ReceivedCallback receivedCallback = nullptr) :
                I2PServiceHandler(parent.get()), 
                _parentTunnel(parent),
                m_DestinationIdentHash(destination),
                m_DestinationPort(destinationPort),
                _connectedCallback(connectedCallback),
                _receivedCallback(receivedCallback)//, m_Socket(socket) 
            {}
            void Handle();
            //void Handle(ClientConnectedCallback clientConnected = nullptr);
            void Terminate();

            // void SetSendCallback(SendCallback sendCallback) { _sendCallback = sendCallback; }
            // SendCallback GetSendCallback() { return _sendCallback; }
            // void SetSendMoreCallback(SendMoreCallback sendMoreCallback) { _sendMoreCallback = sendMoreCallback; }
            // SendMoreCallback GetSendMoreCallback() { return _sendMoreCallback; }

        private:
            void HandleStreamRequestComplete(std::shared_ptr<i2p::stream::Stream> stream);
            i2p::data::IdentHash m_DestinationIdentHash;
            int m_DestinationPort;
            //std::shared_ptr<boost::asio::ip::tcp::socket> m_Socket;
        };

        //void I2PPureClientTunnelHandler::Handle(ClientConnectedCallback clientConnected) 
        //{
        //}
        void I2PPureClientTunnelHandler::Handle()
        {
            GetOwner()->CreateStream(
                std::bind(&I2PPureClientTunnelHandler::HandleStreamRequestComplete, shared_from_this(), std::placeholders::_1),
                m_DestinationIdentHash, m_DestinationPort);
        }

        void I2PPureClientTunnelHandler::HandleStreamRequestComplete(std::shared_ptr<i2p::stream::Stream> stream)
        {
            using namespace std::placeholders;    // adds visibility of _1, _2, _3,...

            if (stream)
            {
                if (Kill()) return;
                LogPrint(eLogDebug, "I2PPureClientTunnelHandler: new connection");

                // this doesn't work
                // auto owner_shared = std::shared_ptr<I2PService>(GetOwner());
                // auto clientTunnel = std::static_pointer_cast<I2PPureClientTunnel>(owner_shared); // GetOwner());
                // // static_cast<I2PPureClientTunnel*>(GetOwner());
                auto clientTunnel = _parentTunnel;

                auto streamCreatedCallback = clientTunnel->GetStreamCreatedCallback();

                if (streamCreatedCallback)
                    streamCreatedCallback(clientTunnel);

                auto connectionCreatedCallback = clientTunnel->GetConnectionCreatedCallback();
                auto receivedCallback = clientTunnel->GetReceivedCallback();
                auto timedOutCallback = clientTunnel->GetTimedOutCallback();
                //auto connectedCallback = clientTunnel->GetConnectedCallback();

                auto connection = std::make_shared<I2PPureTunnelConnection>(
                    GetOwner(), stream, receivedCallback, timedOutCallback); // , connectedCallback
                //auto connection = std::make_shared<I2PPureTunnelConnection>(GetOwner(), m_Socket, stream);
                GetOwner()->AddHandler(connection);

                // the callback can then set the send-callbacks itself as it has everything (but can't send yet, unless we make a good buffer that sticks that info till connected).
                if (connectionCreatedCallback)
                {
                    connectionCreatedCallback(connection);
                }

                auto connectedCallback = clientTunnel->GetConnectedCallback();

                // the question is where we'd want to place the callback (connected), if we do it before we don't 
                // have things ready yet, i.e. we can't send anything and we shouldn't as it'd get lost. The only
                // thing that can go through is some rouge connect (inbound) which shouldn't happen as this is a client
                // and we might be losing some of that received content. Or we'd need to watch on send callbacks set
                // and raise some callback on that which is a bit winded (i.e. we connect before and set send-s after, 
                // but we'd need to notify the 'watcher' that things are then ready. Or buffer and send on connect.

                // std::string request = "I2PPureClientTunnelHandler::HandleStreamRequestComplete.I2PConnect: test something to send, test data...";
                // connection->I2PConnect((const uint8_t *)request.data(), request.length());

                // don't send any test messages as it'd break the server side parsing, just stick to what is normal headers, buffers etc.
                connection->I2PConnect();

                // no longer used by node, node uses connection directly instead to send
                // clientTunnel->SetSendCallback(std::bind(&I2PPureTunnelConnection::HandleSendRawSigned, connection, _1, _2));
                // clientTunnel->SetSendMoreCallback(std::bind(&I2PPureTunnelConnection::HandleSendReadyRawSigned, connection, _1, _2, _3, _4));

                if (connectedCallback) //_connectedCallback)
                {
                    connectedCallback(connection);
                }
                Done(shared_from_this());
            } else
            {
                LogPrint(eLogError, "I2PPureClientTunnelHandler: Client Tunnel Issue when creating the stream, check the previous warnings for more info.");
                Terminate();
            }
        }

        void I2PPureClientTunnelHandler::Terminate()
        {
            if (Kill()) return;
            //if (m_Socket)
            //{
            //	m_Socket->close();
            //	m_Socket = nullptr;
            //}
            Done(shared_from_this());
        }

        I2PPureClientTunnel::I2PPureClientTunnel(const std::string& name, const std::string& destination,
            const std::string& address, int port, std::shared_ptr<ClientDestination> localDestination, int destinationPort, StreamCreatedCallback streamCreated) : //, ClientConnectedCallback clientConnected) :
            TCPIPAcceptor(address, port, localDestination), m_Name(name), m_Destination(destination),
            m_DestinationIdentHash(nullptr), m_DestinationPort(destinationPort), _streamCreated(streamCreated)
            //, _clientConnected(clientConnected)
        {
        }

        void I2PPureClientTunnel::Start()
        {
            // I2PDK: this should be removed (that's the old local-sockets style/waiting)
            // as it stands, acceptor (sockets) won't be initialized (i.e. null) and will be skipped 
            // TCPIPAcceptor::Start();
            
            GetIdentHash();
        }

        void I2PPureClientTunnel::Stop()
        {
            if (IsStopped()){
                LogPrint(eLogWarning, "I2PPureClientTunnel.Stop: already stopped?");
                return;
            }
            TCPIPAcceptor::Stop();
            auto *originalIdentHash = m_DestinationIdentHash;
            m_DestinationIdentHash = nullptr;
            delete originalIdentHash;
            _isStopped = true;
        }

        /* HACK: maybe we should create a caching IdentHash provider in AddressBook */
        const i2p::data::IdentHash * I2PPureClientTunnel::GetIdentHash()
        {
            if (!m_DestinationIdentHash)
            {
                i2p::data::IdentHash identHash;
                if (i2p::client::context.GetAddressBook().GetIdentHash(m_Destination, identHash))
                    m_DestinationIdentHash = new i2p::data::IdentHash(identHash);
                else
                    LogPrint(eLogWarning, "I2PPureClientTunnel: Remote destination ", m_Destination, " not found");
            }
            return m_DestinationIdentHash;
        }

        //std::shared_ptr<I2PServiceHandler> I2PPureClientTunnel::CreateHandler(
        //    std::shared_ptr<boost::asio::ip::tcp::socket> socket,
        //    ClientConnectedCallback clientConnected)
        //{
        //    const i2p::data::IdentHash *identHash = GetIdentHash();
        //    if (identHash)
        //        return  std::make_shared<I2PPureClientTunnelHandler>(
        //            this, *identHash, m_DestinationPort, socket, clientConnected);
        //    else
        //        return nullptr;
        //}
        std::shared_ptr<I2PServiceHandler> I2PPureClientTunnel::CreateHandler(std::shared_ptr<boost::asio::ip::tcp::socket> socket)
        {
            const i2p::data::IdentHash *identHash = GetIdentHash();
            if (identHash) {
                auto s = std::static_pointer_cast<I2PPureClientTunnel>(shared_from_this ());
                // std::shared_ptr<I2PPureClientTunnel>

                auto handler = std::make_shared<I2PPureClientTunnelHandler>(
                    s, *identHash, m_DestinationPort, socket, _connectedCallback, _receivedCallback);
                return handler;
            }
            else
                return nullptr;
        }

        // I2PPureServerTunnel::I2PPureServerTunnel(const std::string& name, const std::string& address, int port,
        //     std::shared_ptr<ClientDestination> localDestination, int inport, bool gzip)
        // {

        // }
        // I2PPureServerTunnel::I2PPureServerTunnel(const std::string& name, const std::string& address, int port,
        //     std::shared_ptr<ClientDestination> localDestination, int inport, bool gzip,
        //     ServerStreamAcceptedCallback acceptedCallback)
        // {

        // }
        I2PPureServerTunnel::I2PPureServerTunnel(const std::string& name, const std::string& address, int port, 
            std::shared_ptr<ClientDestination> localDestination, int inport, bool gzip) :
            I2PService(localDestination), 
            m_IsUniqueLocal(true), 
            m_Name(name), 
            m_Address(address), 
            m_Port(port), 
            m_IsAccessList(false)
        {
            m_PortDestination = localDestination->CreateStreamingDestination(inport > 0 ? inport : port, gzip);
        }
        I2PPureServerTunnel::I2PPureServerTunnel(const std::string& name, const std::string& address, int port, 
            std::shared_ptr<ClientDestination> localDestination, int inport, bool gzip,
            ServerStreamAcceptedCallback acceptedCallback) : 
            I2PPureServerTunnel(name, address, port, localDestination, inport, gzip)
        {
            _acceptedCallback = acceptedCallback;
        }
        // I2PPureServerTunnel::I2PPureServerTunnel(const std::string& name, const std::string& address,
        //     int port, std::shared_ptr<ClientDestination> localDestination, int inport, bool gzip,
        //     ServerStreamAcceptedCallback acceptedCallback) :
        //     I2PService(localDestination), 
        //     m_IsUniqueLocal(true), 
        //     m_Name(name), 
        //     m_Address(address), 
        //     m_Port(port), 
        //     m_IsAccessList(false),
        //     _acceptedCallback(acceptedCallback)
        // {
        //     m_PortDestination = localDestination->CreateStreamingDestination(inport > 0 ? inport : port, gzip);
        // }
        // I2PPureServerTunnel::I2PPureServerTunnel(const std::string& name, const std::string& address,
        //     int port, std::shared_ptr<ClientDestination> localDestination, int inport, bool gzip) : 
        //     I2PPureServerTunnel(name, address, port, localDestination, inport, gzip, nullptr)
        // {
        // }

        // we don't need port/socket stuff here either?
        void I2PPureServerTunnel::Start()
        {
            m_Endpoint.port(m_Port);
            boost::system::error_code ec;
            // this is going to fail as our address is not an ip
            auto addr = boost::asio::ip::address::from_string(m_Address, ec);
            if (!ec)
            {
                m_Endpoint.address(addr);
                Accept();
            } else
            {
                auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(GetService());
                resolver->async_resolve(boost::asio::ip::tcp::resolver::query(m_Address, ""),
                    std::bind(&I2PPureServerTunnel::HandleResolve, this,
                        std::placeholders::_1, std::placeholders::_2, resolver));
            }
        }

        void I2PPureServerTunnel::Stop()
        {
            if (IsStopped()){
                LogPrint(eLogWarning, "I2PPureServerTunnel.Stop: already stopped?");
                return;
            }
            ClearHandlers();
            _isStopped = true;
        }

        void I2PPureServerTunnel::HandleResolve(const boost::system::error_code& ecode, boost::asio::ip::tcp::resolver::iterator it,
            std::shared_ptr<boost::asio::ip::tcp::resolver> resolver)
        {
            if (!ecode)
            {
                auto addr = (*it).endpoint().address();
                LogPrint(eLogInfo, "I2PPureServerTunnel: server tunnel ", (*it).host_name(), " has been resolved to ", addr);
                m_Endpoint.address(addr);
                Accept();
            } else
                LogPrint(eLogError, "I2PPureServerTunnel: Unable to resolve server tunnel address: ", ecode.message());
        }

        void I2PPureServerTunnel::SetAccessList(const std::set<i2p::data::IdentHash>& accessList)
        {
            m_AccessList = accessList;
            m_IsAccessList = true;
        }

        void I2PPureServerTunnel::Accept()
        {
            if (m_PortDestination)
                m_PortDestination->SetAcceptor(std::bind(&I2PPureServerTunnel::HandleAccept, this, std::placeholders::_1));

            auto localDestination = GetLocalDestination();
            if (localDestination)
            {
                if (!localDestination->IsAcceptingStreams()) // set it as default if not set yet
                    localDestination->AcceptStreams(std::bind(&I2PPureServerTunnel::HandleAccept, this, std::placeholders::_1));
            } else
                LogPrint(eLogError, "I2PPureServerTunnel: Local destination not set for server tunnel");
        }

        // this is what gets called on a new incoming i2p stream (connection is created here).
        void I2PPureServerTunnel::HandleAccept(std::shared_ptr<i2p::stream::Stream> stream)
        {
            using namespace std::placeholders;    // adds visibility of _1, _2, _3,...

            if (stream)
            {
                std::string remoteIdentity = stream->GetRemoteIdentity()->GetIdentHash().ToBase32();
                if (m_IsAccessList)
                {
                    if (!m_AccessList.count(stream->GetRemoteIdentity()->GetIdentHash()))
                    {
                        LogPrint(eLogWarning, "I2PPureServerTunnel: Address ", remoteIdentity, " is not in white list. Incoming connection dropped");
                        stream->Close();
                        return;
                    }
                }

                LogPrint(eLogDebug, "I2PPureServerTunnel: Address ", remoteIdentity, " is now streaming...");

                //std::static_pointer_cast<I2PPureServerTunnel>(shared_from_this ())
                auto serverTunnel = std::static_pointer_cast<I2PPureServerTunnel>(shared_from_this());

                auto acceptedCallback = serverTunnel->GetAcceptedCallback();

                // different approach for setting up callbacks (for the server and after accepted, i.e. all connection specific)
                // pass refs and get the callbacks set up during the accepted callback, as those callbacks are NOT 'per tunnel'
                // but instead are 'per connection' (i.e. we can't use the serverTunnel-> pattern as w/ others).
                // Connection created and connected are one-off used only here so that's fine (we don't need to store it),
                // and the receive callback is saved within the connection, and only used from in there, so again nothing to save
                // (i.e. no need for 'std::vector<std::pair<ReceivedCallback, uint32_t> > _receivedCallbacks;', see I2PService.h)
                ServerConnectionCreatedCallback connectionCreatedCallback;
                ServerClientConnectedCallback connectedCallback;
                ReceivedCallback receivedCallback;
                ConnectionTimedOutCallback timedOutCallback;

                // put this before Connect, prepare others to receive in case a stream gets connected right away.
                // Since we're a server now, we should be ready before and we probably won't be sending anything before
                // anyone connects (or are we? we may have part client part server scenarios thought not typically).
                if (acceptedCallback)
                    acceptedCallback(
                        serverTunnel, 
                        remoteIdentity,
                        connectionCreatedCallback,
                        connectedCallback,
                        receivedCallback,
                        timedOutCallback);

                // // now the other callbacks should be ready, as we're linking those to the node (set on accept above)
                // auto connectionCreatedCallback = serverTunnel->GetConnectionCreatedCallback();
                // auto connectedCallback = serverTunnel->GetConnectedCallback();
                // auto receivedCallback = serverTunnel->GetReceivedCallback();

                // new connection, we only need received inside
                auto conn = CreateI2PConnection(stream, receivedCallback, timedOutCallback);
                AddHandler(conn);
                // TODO: we should add some callback when the handler is removed - as that happens regularly for connection,
                // connection gets terminated from inside and we're neve notified seems.
                // For now we're just making check from the Node whether connection->IsStreamAlive()

                if (connectionCreatedCallback)
                    connectionCreatedCallback(serverTunnel, conn);

                // this or shared_from_this()
                // or just set our own callbacks and let tunnel handle us when sending
                // or just let the callback set that info from the tunnel/connection, it has everything
                // //SetSendCallback(std::bind(&I2PPureTunnelConnection::HandleSendRawSigned, conn, _1, _2));
                // this is now moved and called directly from the neti2pd:TunnelSendData() as we have the connection.
                // SetSendMoreCallback(std::bind(&I2PPureTunnelConnection::HandleSendReadyRawSigned, conn, _1, _2, _3, _4));

                // this is the 'other' Connect for the server side (client uses I2PConnect).
                conn->Connect(m_IsUniqueLocal);

                if (connectedCallback) //_connectedCallback)
                {
                    connectedCallback(serverTunnel, conn);
                }
            }
        }

        std::shared_ptr<I2PPureTunnelConnection> I2PPureServerTunnel::CreateI2PConnection(
            std::shared_ptr<i2p::stream::Stream> stream, 
            ReceivedCallback receivedCallback,
            ConnectionTimedOutCallback timedOutCallback)
        {
            // return std::make_shared<I2PPureTunnelConnection>(this, stream, GetEndpoint(), receivedCallback, false);
            // turn quiet on as it was producing errors (turned in code as well)
            return std::make_shared<I2PPureTunnelConnection>(this, stream, GetEndpoint(), receivedCallback, timedOutCallback); //, false);
        }


    }
}
