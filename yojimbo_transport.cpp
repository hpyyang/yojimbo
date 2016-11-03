/*
    Yojimbo Client/Server Network Protocol Library.

    Copyright © 2016, The Network Protocol Company, Inc.

    Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

        1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

        2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer 
           in the documentation and/or other materials provided with the distribution.

        3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived 
           from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
    INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
    WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
    USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "yojimbo_transport.h"
#include "yojimbo_common.h"
#include <stdint.h>
#include <inttypes.h>

namespace yojimbo
{
    BaseTransport::BaseTransport( Allocator & allocator, 
                                  const Address & address,
                                  uint32_t protocolId,
                                  int maxPacketSize, 
                                  int sendQueueSize, 
                                  int receiveQueueSize )
    
        : m_sendQueue( allocator, sendQueueSize ),
          m_receiveQueue( allocator, receiveQueueSize )
    {
        assert( protocolId != 0 );
        assert( sendQueueSize > 0 );
        assert( receiveQueueSize > 0 );

        m_address = address;

        m_time = 0.0;

        m_flags = 0;

        m_context = NULL;

        m_allocator = &allocator;
                
        m_streamAllocator = &allocator;

        m_protocolId = protocolId;

        m_packetFactory = NULL;
        
        m_packetProcessor = YOJIMBO_NEW( allocator, PacketProcessor, allocator, m_protocolId, maxPacketSize );
        
        memset( m_counters, 0, sizeof( m_counters ) );

#if YOJIMBO_INSECURE_CONNECT
        m_allPacketTypes = NULL;
#endif // #if YOJIMBO_INSECURE_CONNECT
        m_packetTypeIsEncrypted = NULL;
        m_packetTypeIsUnencrypted = NULL;

		m_contextManager = YOJIMBO_NEW( allocator, ContextManager );

		m_encryptionManager = YOJIMBO_NEW( allocator, EncryptionManager );
	}

    void BaseTransport::SetPacketFactory( PacketFactory & packetFactory )
    {
        assert( m_packetFactory == NULL );

        m_packetFactory = &packetFactory;

        const int numPacketTypes = m_packetFactory->GetNumPacketTypes();

        assert( numPacketTypes > 0 );

#if YOJIMBO_INSECURE_CONNECT
        m_allPacketTypes = (uint8_t*) m_allocator->Allocate( numPacketTypes );
#endif // #if YOJIMBO_INSECURE_CONNECT

        m_packetTypeIsEncrypted = (uint8_t*) m_allocator->Allocate( numPacketTypes );
        m_packetTypeIsUnencrypted = (uint8_t*) m_allocator->Allocate( numPacketTypes );

#if YOJIMBO_INSECURE_CONNECT
        memset( m_allPacketTypes, 1, m_packetFactory->GetNumPacketTypes() );
#endif // #if YOJIMBO_INSECURE_CONNECT
        memset( m_packetTypeIsEncrypted, 0, m_packetFactory->GetNumPacketTypes() );
        memset( m_packetTypeIsUnencrypted, 1, m_packetFactory->GetNumPacketTypes() );
    }

    void BaseTransport::ClearPacketFactory()
    {
		if ( !m_packetFactory )
			return;

        ClearSendQueue();
        ClearReceiveQueue();

#if YOJIMBO_INSECURE_CONNECT
        m_allocator->Free( m_allPacketTypes );
#endif // #if YOJIMBO_INSECURE_CONNECT
        m_allocator->Free( m_packetTypeIsEncrypted );
        m_allocator->Free( m_packetTypeIsUnencrypted );

        m_packetFactory = NULL;
#if YOJIMBO_INSECURE_CONNECT
        m_allPacketTypes = NULL;
#endif // #if YOJIMBO_INSECURE_CONNECT
        m_packetTypeIsEncrypted = NULL;
        m_packetTypeIsUnencrypted = NULL;

        m_packetFactory = NULL;
    }

    BaseTransport::~BaseTransport()
    {
        ClearPacketFactory();

        YOJIMBO_DELETE( *m_allocator, PacketProcessor, m_packetProcessor );
		YOJIMBO_DELETE( *m_allocator, ContextManager, m_contextManager );
		YOJIMBO_DELETE( *m_allocator, EncryptionManager, m_encryptionManager );

        m_allocator = NULL;
    }

    void BaseTransport::Reset()
    {
        ClearSendQueue();
        ClearReceiveQueue();
        ResetContextMappings();
        ResetEncryptionMappings();
    }

    void BaseTransport::ClearSendQueue()
    {
        for ( int i = 0; i < m_sendQueue.GetNumEntries(); ++i )
        {
            PacketEntry & entry = m_sendQueue[i];
            assert( entry.packet );
            assert( entry.packet->IsValid() );
            assert( entry.address.IsValid() );
            entry.packet->Destroy();
            entry.address = Address();
            entry.packet = NULL;
        }

        m_sendQueue.Clear();
    }

    void BaseTransport::ClearReceiveQueue()
    {
        for ( int i = 0; i < m_receiveQueue.GetNumEntries(); ++i )
        {
            PacketEntry & entry = m_receiveQueue[i];
            assert( entry.packet );
            assert( entry.packet->IsValid() );
            assert( entry.address.IsValid() );
            entry.packet->Destroy();
            entry.address = Address();
            entry.packet = NULL;
        }

        m_receiveQueue.Clear();
    }

    Packet * BaseTransport::CreatePacket( int type )
    {
        assert( m_packetFactory );
        return m_packetFactory->CreatePacket( type );
    }

    void BaseTransport::SendPacket( const Address & address, Packet * packet, uint64_t sequence, bool immediate )
    {
        assert( m_allocator );
        assert( m_packetFactory );

        assert( packet );
        assert( packet->IsValid() );
        assert( address.IsValid() );

        if ( immediate )
        {
            m_counters[TRANSPORT_COUNTER_PACKETS_SENT]++;

            WriteAndFlushPacket( address, packet, sequence );

            packet->Destroy();
        }
        else
        {
            if ( !m_sendQueue.IsFull() )
            {
                PacketEntry entry;
                entry.sequence = sequence;
                entry.address = address;
                entry.packet = packet;

                m_sendQueue.Push( entry );
            }
            else
            {
                debug_printf( "base transport send queue overflow\n" );
                m_counters[TRANSPORT_COUNTER_SEND_QUEUE_OVERFLOW]++;
                packet->Destroy();
                return;
            }
        }

        m_counters[TRANSPORT_COUNTER_PACKETS_SENT]++;
    }

    Packet * BaseTransport::ReceivePacket( Address & from, uint64_t * sequence )
    {
        if ( !m_packetFactory )
            return NULL;

        assert( m_allocator );
        assert( m_packetFactory );

        if ( m_receiveQueue.IsEmpty() )
            return NULL;

        PacketEntry entry = m_receiveQueue.Pop();

        assert( entry.packet );
        assert( entry.packet->IsValid() );
        assert( entry.address.IsValid() );

        from = entry.address;

        if ( sequence )
            *sequence = entry.sequence;

        m_counters[TRANSPORT_COUNTER_PACKETS_RECEIVED]++;

        return entry.packet;
    }

    void BaseTransport::WritePackets()
    {
        if ( !m_packetFactory )
            return;

        assert( m_allocator );
        assert( m_packetFactory );
        assert( m_packetProcessor );

        while ( !m_sendQueue.IsEmpty() )
        {
            PacketEntry entry = m_sendQueue.Pop();

            assert( entry.packet );
            assert( entry.packet->IsValid() );
            assert( entry.address.IsValid() );

            WriteAndFlushPacket( entry.address, entry.packet, entry.sequence );

            entry.packet->Destroy();
        }
    }

    void BaseTransport::WriteAndFlushPacket( const Address & address, Packet * packet, uint64_t sequence )
    {
        assert( m_packetFactory );

        assert( packet );
        assert( packet->IsValid() );
        assert( address.IsValid() );

        const int packetType = packet->GetType();

        assert( packetType >= 0 );
        assert( packetType < m_packetFactory->GetNumPacketTypes() );

        const uint8_t * key = m_encryptionManager->GetSendKey( address, GetTime() );

#if YOJIMBO_INSECURE_CONNECT
        const bool encrypt = ( GetFlags() & TRANSPORT_FLAG_INSECURE_MODE ) ? IsEncryptedPacketType( packetType ) && key : IsEncryptedPacketType( packetType );
#else // #if YOJIMBO_INSECURE_CONNECT
        const bool encrypt = IsEncryptedPacketType( packetType );
#endif // #if YOJIMBO_INSECURE_CONNECT

        int packetBytes;

        const Context * context = m_contextManager->GetContext( address );

        Allocator * streamAllocator = context ? context->streamAllocator : m_streamAllocator;
        PacketFactory * packetFactory = context ? context->packetFactory : m_packetFactory;

        assert( streamAllocator );
        assert( packetFactory );
        assert( packetFactory->GetNumPacketTypes() == m_packetFactory->GetNumPacketTypes() );

        m_packetProcessor->SetContext( context ? context->contextData : m_context );

        const uint8_t * packetData = m_packetProcessor->WritePacket( packet, sequence, packetBytes, encrypt, key, *streamAllocator, *packetFactory );

        if ( !packetData )
        {
            switch ( m_packetProcessor->GetError() )
            {
                case PACKET_PROCESSOR_ERROR_KEY_IS_NULL:                
                {
                    debug_printf( "base transport packet processor key is null (write packet)\n" );
                    m_counters[TRANSPORT_COUNTER_ENCRYPTION_MAPPING_FAILURES]++;         
                }
                break;

                case PACKET_PROCESSOR_ERROR_ENCRYPT_FAILED:
                {
                    debug_printf( "base transport encrypt failed (write packet)\n" );
                    m_counters[TRANSPORT_COUNTER_ENCRYPT_PACKET_FAILURES]++;
                }
                break;

                case PACKET_PROCESSOR_ERROR_WRITE_PACKET_FAILED:
                {
                    debug_printf( "base transport write packet failed (write packet)\n" );
                    m_counters[TRANSPORT_COUNTER_WRITE_PACKET_FAILURES]++;               
                }
                break;

                default:
                    break;
            }

            return;
        }

        InternalSendPacket( address, packetData, packetBytes );

        m_counters[TRANSPORT_COUNTER_PACKETS_WRITTEN]++;

        if ( encrypt )
            m_counters[TRANSPORT_COUNTER_ENCRYPTED_PACKETS_WRITTEN]++;
        else
            m_counters[TRANSPORT_COUNTER_UNENCRYPTED_PACKETS_WRITTEN]++;
    }

    void BaseTransport::ReadPackets()
    {
        if ( !m_packetFactory )
            return;

        assert( m_allocator );
        assert( m_packetFactory );
        assert( m_packetProcessor );

        const int maxPacketSize = GetMaxPacketSize();

        uint8_t * packetBuffer = (uint8_t*) alloca( maxPacketSize );

        while ( true )
        {
            Address address;
            int packetBytes = InternalReceivePacket( address, packetBuffer, maxPacketSize );
            if ( !packetBytes )
                break;

            assert( packetBytes > 0 );

            if ( m_receiveQueue.IsFull() )
            {
                debug_printf( "base transport receive queue overflow\n" );
                m_counters[TRANSPORT_COUNTER_RECEIVE_QUEUE_OVERFLOW]++;
                break;
            }

            bool encrypted = false;

            const uint8_t * encryptedPacketTypes = m_packetTypeIsEncrypted;
            const uint8_t * unencryptedPacketTypes = m_packetTypeIsUnencrypted;

#if YOJIMBO_INSECURE_CONNECT
            if ( GetFlags() & TRANSPORT_FLAG_INSECURE_MODE )
            {
                encryptedPacketTypes = m_allPacketTypes;
                unencryptedPacketTypes = m_allPacketTypes;
            }
#endif // #if YOJIMBO_INSECURE_CONNECT

            const uint8_t * key = m_encryptionManager->GetReceiveKey( address, GetTime() );
           
            uint64_t sequence = 0;

            const Context * context = m_contextManager->GetContext( address );

            Allocator * streamAllocator = context ? context->streamAllocator : m_streamAllocator;
            PacketFactory * packetFactory = context ? context->packetFactory : m_packetFactory;
    
            assert( streamAllocator );
            assert( packetFactory );
            assert( packetFactory->GetNumPacketTypes() == m_packetFactory->GetNumPacketTypes() );

            m_packetProcessor->SetContext( context ? context->contextData : m_context );

            Packet * packet = m_packetProcessor->ReadPacket( packetBuffer, sequence, packetBytes, encrypted, key, encryptedPacketTypes, unencryptedPacketTypes, *streamAllocator, *packetFactory );

            if ( !packet )
            {
                switch ( m_packetProcessor->GetError() )
                {
                    case PACKET_PROCESSOR_ERROR_KEY_IS_NULL:
                    {
                        debug_printf( "base transport key is null (read packet)\n" );
                        m_counters[TRANSPORT_COUNTER_ENCRYPTION_MAPPING_FAILURES]++;
                    }
                    break;

                    case PACKET_PROCESSOR_ERROR_DECRYPT_FAILED:
                    {
                        debug_printf( "base transport decrypt failed (read packet)\n" );
                        m_counters[TRANSPORT_COUNTER_ENCRYPT_PACKET_FAILURES]++;
                    }
                    break;

                    case PACKET_PROCESSOR_ERROR_PACKET_TOO_SMALL:
                    {
                        debug_printf( "base transport packet too small (read packet)\n" );
                        m_counters[TRANSPORT_COUNTER_DECRYPT_PACKET_FAILURES]++;
                    }
                    break;

                    case PACKET_PROCESSOR_ERROR_READ_PACKET_FAILED:
                    {
                        debug_printf( "base transport read packet failed (read packet)\n" );
                        m_counters[TRANSPORT_COUNTER_READ_PACKET_FAILURES]++;
                    }
                    break;

                    default:
                        break;
                }

                continue;
            }

            PacketEntry entry;
            entry.sequence = sequence;
            entry.packet = packet;
            entry.address = address;

            m_receiveQueue.Push( entry );

            m_counters[TRANSPORT_COUNTER_PACKETS_READ]++;

            if ( encrypted )
                m_counters[TRANSPORT_COUNTER_ENCRYPTED_PACKETS_READ]++;
            else
                m_counters[TRANSPORT_COUNTER_UNENCRYPTED_PACKETS_READ]++;
        }
    }

    int BaseTransport::GetMaxPacketSize() const 
    {
        return m_packetProcessor->GetMaxPacketSize();
    }

    void BaseTransport::SetContext( void * context )
    {
        m_context = context;
    }

    void BaseTransport::SetStreamAllocator( Allocator & allocator )
    {
        m_streamAllocator = &allocator;
    }

    void BaseTransport::EnablePacketEncryption()
    {
        assert( m_packetFactory );
        memset( m_packetTypeIsEncrypted, 1, m_packetFactory->GetNumPacketTypes() );
        memset( m_packetTypeIsUnencrypted, 0, m_packetFactory->GetNumPacketTypes() );
    }

    void BaseTransport::DisableEncryptionForPacketType( int type )
    {
        assert( m_packetFactory );
        assert( type >= 0 );
        assert( type < m_packetFactory->GetNumPacketTypes() );
        m_packetTypeIsEncrypted[type] = 0;
        m_packetTypeIsUnencrypted[type] = 1;
    }

    bool BaseTransport::IsEncryptedPacketType( int type ) const
    {
        assert( m_packetFactory );
        assert( type >= 0 );
        assert( type < m_packetFactory->GetNumPacketTypes() );
        return m_packetTypeIsEncrypted[type] != 0;
    }

    bool BaseTransport::AddEncryptionMapping( const Address & address, const uint8_t * sendKey, const uint8_t * receiveKey )
    {
        return m_encryptionManager->AddEncryptionMapping( address, sendKey, receiveKey, GetTime() );
    }

    bool BaseTransport::RemoveEncryptionMapping( const Address & address )
    {
        return m_encryptionManager->RemoveEncryptionMapping( address, GetTime() );
    }

    void BaseTransport::ResetEncryptionMappings()
    {
        m_encryptionManager->ResetEncryptionMappings();
    }

    bool BaseTransport::AddContextMapping( const Address & address, Allocator & streamAllocator, PacketFactory & packetFactory, void * contextData )
    {
        return m_contextManager->AddContextMapping( address, streamAllocator, packetFactory, contextData );
    }

    bool BaseTransport::RemoveContextMapping( const Address & address )
    {
        return m_contextManager->RemoveContextMapping( address );
    }

    void BaseTransport::ResetContextMappings()
    {
        m_contextManager->ResetContextMappings();
    }

    void BaseTransport::AdvanceTime( double time )
    {
        assert( time >= m_time );
        m_time = time;
    }

    double BaseTransport::GetTime() const
    {
        return m_time;
    }

    uint64_t BaseTransport::GetCounter( int index ) const
    {
        assert( index >= 0 );
        assert( index < TRANSPORT_COUNTER_NUM_COUNTERS );
        return m_counters[index];
    }

    void BaseTransport::SetFlags( uint64_t flags )
    {
        m_flags = flags;
    }

    uint64_t BaseTransport::GetFlags() const
    {
        return m_flags;
    }

    const Address & BaseTransport::GetAddress() const
    {
        return m_address;
    }

    PacketFactory * BaseTransport::GetPacketFactory()
    {
        return m_packetFactory;
    }

    const PacketFactory * BaseTransport::GetPacketFactory() const
    {
        return m_packetFactory;
    }
}
