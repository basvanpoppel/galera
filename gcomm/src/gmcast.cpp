#include "gmcast.hpp"

#include "gcomm/common.hpp"
#include "gcomm/conf.hpp"
#include "gcomm/util.hpp"
#include "gcomm/map.hpp"
#include "defaults.hpp"

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/kruskal_min_spanning_tree.hpp>

using namespace std;
using namespace std::rel_ops;
using namespace gcomm;
using namespace gu;
using namespace gu::net;
using namespace gu::datetime;

// map file descriptor to connection context
class gcomm::GMCastProtoMap : 
    public Map<const int, GMCastProto*, std::map<const int, GMCastProto*> >
{};

class gcomm::UUIDToAddressMap :
    public MultiMap<const UUID, string, std::multimap<const UUID, string> >
{};

class gcomm::GMCastNode
{
    bool    operational;
    uint8_t weight;
    UUID    uuid;
    static const size_t ADDR_SIZE = 64;
    String<ADDR_SIZE> address;
    
public:
    
    GMCastNode()
        : 
        operational(false), 
        weight(0xff), 
        uuid(),
        address()
    { }
    
    GMCastNode(const bool    operational_, 
               const UUID&   uuid_, 
               const string& address_)
        :
        operational (operational_), 
        weight      (0xff),
        uuid        (uuid_),
        address     (address_)
    {
        if (address_.size() > ADDR_SIZE)
        {
            gcomm_throw_fatal << "Address too big: " << address_.size()
                              << " > " << (sizeof(address));
        }
    }
    
    void set_operational(bool op) { operational = op; }
    
    bool is_operational() const   { return operational; }
    
    const UUID& get_uuid() const  { return uuid; }
    
    size_t unserialize(const byte_t* buf, const size_t buflen, const size_t offset)
    {
        size_t  off;
        uint8_t byte;
        
        gu_trace (off = gcomm::unserialize(buf, buflen, offset, &byte));
        
        operational = byte & 0x1;
        
        gu_trace (off = uuid.unserialize(buf, buflen, off));
        gu_trace (off = address.unserialize(buf, buflen, off));

        return off;
    }
    
    size_t serialize(byte_t* buf, const size_t buflen, const size_t offset) const
    {
        size_t  off;
        uint8_t byte = static_cast<uint8_t>(operational ? 0x1 : 0);
        
        gu_trace (off = gcomm::serialize(byte, buf, buflen, offset));
        gu_trace (off = uuid.serialize(buf, buflen, off));
        gu_trace (off = address.serialize(buf, buflen, off));
        
        return off;
    }
    
    const string& get_address() const { return address.to_string(); }
    
    static size_t serial_size() 
    { return (1 + UUID::serial_size() + ADDR_SIZE); }
};


class gcomm::GMCastMessage
{
public:

    enum Flags {
        F_GROUP_NAME   = 1 << 0,
        F_NODE_NAME    = 1 << 1,
        F_NODE_ADDRESS = 1 << 2,
        F_NODE_LIST    = 1 << 3
    };
    
    enum Type 
    {
        T_INVALID            = 0,
        T_HANDSHAKE          = 1,
        T_HANDSHAKE_RESPONSE = 2,
        T_HANDSHAKE_OK       = 3,
        T_HANDSHAKE_FAIL     = 4,
        T_TOPOLOGY_CHANGE    = 5,
        /* Leave room for future use */
        T_USER_BASE          = 8,
        T_MAX
    };

    typedef std::list<GMCastNode> NodeList;

private:

    byte_t version;
    Type   type;
    byte_t flags;
    byte_t ttl;
    UUID   source_uuid;
    String<64> node_address;
    String<32> group_name;


    GMCastMessage& operator=(const GMCastMessage&);

    NodeList* node_list; // @todo: since we do a full node list copy in ctor
                         //        below, do we really need a pointer here?

public:
    
    static const char* type_to_string (Type t)
    {
        static const char* str[T_MAX] =
        {
            "INVALID",
            "HANDSHAKE",
            "HANDSHAKE_RESPONSE",
            "HANDSHAKE_OK",
            "HANDSHAKE_FAIL",
            "TOPOLOGY_CHANGE",
            "RESERVED_6",
            "RESERVED_7",
            "USER_BASE"
        };
        
        if (T_MAX > t) return str[t];

        return "UNDEFINED PACKET TYPE";
    }

    GMCastMessage(const GMCastMessage& msg) :
        version      (msg.version),
        type         (msg.type),
        flags        (msg.flags),
        ttl          (msg.ttl),
        source_uuid  (msg.source_uuid),
        node_address (msg.node_address),
        group_name   (msg.group_name),
        node_list    (msg.node_list != 0 ? new NodeList(*msg.node_list) : 0)
    { }
    /* Default ctor */
    GMCastMessage ()
        :
        version      (0),
        type         (T_INVALID),
        flags        (0),
        ttl          (0),
        source_uuid  (),
        node_address (),
        group_name   (),
        node_list    (0)
    {}
    
    /* Ctor for handshake, handshake ok and handshake fail */
    GMCastMessage (const Type  type_,
                   const UUID& source_uuid_)
        :
        version      (0), 
        type         (type_), 
        flags        (0), 
        ttl          (1), 
        source_uuid  (source_uuid_), 
        node_address (),
        group_name   (),
        node_list    (0)
    {
        if (type != T_HANDSHAKE && type != T_HANDSHAKE_OK && 
            type != T_HANDSHAKE_FAIL)
            gcomm_throw_fatal << "Invalid message type " << type_to_string(type)
                              << " in handshake constructor";        
    }
    
    /* Ctor for user message */
    GMCastMessage (const Type    type_,
                   const UUID&   source_uuid_, 
                   const uint8_t ttl_)
        :
        version      (0), 
        type         (type_), 
        flags        (0), 
        ttl          (ttl_), 
        source_uuid  (source_uuid_), 
        node_address (),
        group_name   (""),
        node_list    (0)
    {
        if (type < T_USER_BASE)
            gcomm_throw_fatal << "Invalid message type " << type_to_string(type)
                              << " in user message constructor";
    }
    
    /* Ctor for handshake response */
    GMCastMessage (const Type    type_,
                   const UUID&   source_uuid_,
                   const string& node_address_,
                   const string& group_name_)
        : 
        version      (0),
        type         (type_), 
        flags        (F_GROUP_NAME | F_NODE_ADDRESS), 
        ttl          (1),
        source_uuid  (source_uuid_),
        node_address (node_address_),
        group_name   (group_name_),
        node_list    (0)

    {
        if (type != T_HANDSHAKE_RESPONSE)
            gcomm_throw_fatal << "Invalid message type " << type_to_string(type)
                              << " in handshake response constructor";
    }

    /* Ctor for topology change */
    GMCastMessage (const Type      type_, 
                   const UUID&     source_uuid_,
                   const string&   group_name_,
                   const NodeList& nodes)
        :
        version      (0),
        type         (type_),
        flags        (F_GROUP_NAME | F_NODE_LIST), 
        ttl          (1),
        source_uuid  (source_uuid_),
        node_address (),
        group_name   (group_name_),
        node_list    (new NodeList(nodes))
    {
        if (type != T_TOPOLOGY_CHANGE)
            gcomm_throw_fatal << "Invalid message type " << type_to_string(type)
                              << " in topology change constructor";
    }
    
    ~GMCastMessage() 
    {
        delete node_list;
    }


    size_t serialize(byte_t* buf, const size_t buflen, const size_t offset) const
        throw (gu::Exception)
    {
        size_t off;

        gu_trace (off = gcomm::serialize(version, buf, buflen, offset));
        gu_trace (off = gcomm::serialize(static_cast<byte_t>(type),buf,buflen,off));
        gu_trace (off = gcomm::serialize(flags, buf, buflen, off));
        gu_trace (off = gcomm::serialize(ttl, buf, buflen, off));
        gu_trace (off = source_uuid.serialize(buf, buflen, off));
        
        if (flags & F_NODE_ADDRESS)
        {
            gu_trace (off = node_address.serialize(buf, buflen, off));
        }
        
        if (flags & F_GROUP_NAME) 
        {
            gu_trace (off = group_name.serialize(buf, buflen, off));
        }
        
        if (flags & F_NODE_LIST) 
        {
            gu_trace (off = gcomm::serialize(
                          static_cast<uint16_t>(node_list->size()),
                          buf, buflen, off));
            
            for (NodeList::const_iterator i = node_list->begin();
                 i != node_list->end(); ++i) 
            {
                gu_trace (off = i->serialize(buf, buflen, off));
            }
        }
        return off;
    }
    
    size_t read_v0(const byte_t* buf, const size_t buflen, const size_t offset)
        throw (gu::Exception)
    {
        size_t off;
        byte_t t;

        gu_trace (off = gcomm::unserialize(buf, buflen, offset, &t));
        type = static_cast<Type>(t);
        gu_trace (off = gcomm::unserialize(buf, buflen, off, &flags));
        gu_trace (off = gcomm::unserialize(buf, buflen, off, &ttl));
        gu_trace (off = source_uuid.unserialize(buf, buflen, off));
        
        if (flags & F_NODE_ADDRESS)
        {
            gu_trace (off = node_address.unserialize(buf, buflen, off));
        }
        
        if (flags & F_GROUP_NAME) 
        {
            gu_trace (off = group_name.unserialize(buf, buflen, off));
        }
        
        if (flags & F_NODE_LIST)
        {
            uint16_t size;

            gu_trace (off = gcomm::unserialize(buf, buflen, off, &size));

            node_list = new NodeList(); // @todo: danger! Prev. list not deleted

            for (uint16_t i = 0; i < size; ++i) 
            {
                GMCastNode node;

                gu_trace (off = node.unserialize(buf, buflen, off));
                node_list->push_back(node);
            }
        }

        return off;
    }
    
    size_t unserialize(const byte_t* buf, const size_t buflen, const size_t offset)
        throw (gu::Exception)
    {
        size_t off;
        
        gu_trace (off = gcomm::unserialize(buf, buflen, offset, &version));
        
        switch (version) {
        case 0:
            gu_trace (return read_v0(buf, buflen, off));
        default:
            return 0;
        }
    }
    
    size_t serial_size() const
    {
        return 4 /* Common header: version, type, flags, ttl */ 
            + source_uuid.serial_size()
            /* GMCast address if set */
            + (flags & F_NODE_ADDRESS ? node_address.serial_size() : 0)
            /* Group name if set */
            + (flags & F_GROUP_NAME ? group_name.serial_size() : 0)
            /* Node list if set */
            + (flags & F_NODE_LIST ? 
               2 + node_list->size()*GMCastNode::serial_size() : 0);
    }
    
    uint8_t get_version() const { return version; }
    
    Type    get_type()    const { return type;    }

    uint8_t get_flags()   const { return flags;   }

    uint8_t get_ttl()     const { return ttl;     }
    
    void dec_ttl()
    {
        if (ttl == 0) gcomm_throw_fatal << "Decrementing 0 ttl";
        ttl--;
    }
    
    const UUID&     get_source_uuid()  const { return source_uuid;  }

    const string&   get_node_address() const { return node_address.to_string(); }

    const string&   get_group_name()   const { return group_name.to_string();   }

    const NodeList* get_node_list()    const { return node_list;    }
};



static bool exists(const UUIDToAddressMap& uuid_map, 
                   const UUID& uuid,
                   const string& addr)
{
    pair<UUIDToAddressMap::const_iterator, 
        UUIDToAddressMap::const_iterator> 
        ret = uuid_map.equal_range(uuid);
    for (UUIDToAddressMap::const_iterator i = ret.first; 
         i != ret.second; ++i)
    {
        if (UUIDToAddressMap::get_value(i) == addr)
            return true;
    }
    return false;
}

static bool equals(const UUIDToAddressMap& a, 
                   const UUIDToAddressMap& b)
{
    if (a.size() != b.size())
        return false;
    for (UUIDToAddressMap::const_iterator i = a.begin(); i != a.end(); ++i)
    {
        if (exists(b, UUIDToAddressMap::get_key(i), 
                   UUIDToAddressMap::get_value(i)) == false)
            return false;
    }
    return true;
}

static void set_tcp_defaults (URI* uri)
{
    // what happens if there is already this parameter?
    uri->set_query_param(Conf::TcpParamNonBlocking, gu::to_string(1));
}

class gcomm::GMCastProto 
{
public:
    
    enum State 
    {
        S_INIT,
        S_HANDSHAKE_SENT,
        S_HANDSHAKE_WAIT,
        S_HANDSHAKE_RESPONSE_SENT,
        S_OK,
        S_FAILED,
        S_CLOSED
    };

private:

    UUID    local_uuid;  // @todo: do we need it here?
    UUID    remote_uuid;
    string  local_addr;
    string  remote_addr;
    string  group_name;
    uint8_t send_ttl;
    bool    changed;
    State   state;

    bool propagate_remote;

    Transport*       tp;
    UUIDToAddressMap uuid_map;

    GMCastProto(const GMCastProto&);
    GMCastProto& operator=(const GMCastProto&);

public:
    
    State get_state() const 
    {
        return state;
    }

    static string to_string (State s) 
    {
        switch (s)
        {
        case S_INIT:                    return "INIT";
        case S_HANDSHAKE_SENT:          return "HANDSHAKE_SENT";
        case S_HANDSHAKE_WAIT:          return "HANDSHAKE_WAIT";
        case S_HANDSHAKE_RESPONSE_SENT: return "HANDSHAKE_RESPONSE_SENT";
        case S_OK:                      return "OK";
        case S_FAILED:                  return "FAILED";
        case S_CLOSED:                  return "CLOSED";
        default: return "UNKNOWN";
        }
    }
    
    void set_state(State new_state) 
    {
        log_debug << "State change: " << to_string(state) << " -> " 
                  << to_string(new_state);

        static const bool allowed[][7] =
        {
            // INIT  HS_SENT HS_WAIT HSR_SENT   OK    FAILED CLOSED
            { false,  true,   true,   false,  false,  false, false },// INIT

            { false,  false,  false,  false,  true,   true,  false },// HS_SENT

            { false,  false,  false,  true,   false,  false, false },// HS_WAIT

            { false,  false,  false,  false,  true,   true,  false },// HSR_SENT

            { false,  false,  false,  false,  false,  true,  true  },// OK

            { false,  false,  false,  false,  false,  false, true  },// FAILED

            { false,  false,  false,  false,  false,  false, false } // CLOSED
        };

        if (!allowed[state][new_state])
        {
            gcomm_throw_fatal << "Invalid state change: " << to_string(state)
                              << " -> " << to_string(new_state);
        }

        state = new_state;
    }
    
    GMCastProto (Transport*    tp_, 
                 const string& local_addr_, 
                 const string& remote_addr_, 
                 const UUID&   local_uuid_, 
                 const string& group_name_)
        : 
        local_uuid       (local_uuid_),
        remote_uuid      (),
        local_addr       (local_addr_),
        remote_addr      (remote_addr_),
        group_name       (group_name_),
        send_ttl         (1),
        changed          (false),
        state            (S_INIT),
        propagate_remote (false),
        tp               (tp_),
        uuid_map         ()
    { }

    ~GMCastProto() 
    {
        delete tp;
    }

    void send_msg(const GMCastMessage& msg)
    {
        Buffer buf;
        gu_trace(serialize(msg, buf));
        int ret = tp->handle_down(Datagram(buf), 0);

        if (ret)
        {
            log_debug << "Send failed: " << strerror(ret);
            set_state(S_FAILED);
        }
    }
    
    void send_handshake() 
    {
        GMCastMessage hs (GMCastMessage::T_HANDSHAKE, local_uuid);

        send_msg(hs);

        set_state(S_HANDSHAKE_SENT);
    }
    
    void wait_handshake() 
    {
        if (get_state() != S_INIT)
            gcomm_throw_fatal << "Invalid state: " << to_string(get_state());
        
        set_state(S_HANDSHAKE_WAIT);
    }
    
    void handle_handshake(const GMCastMessage& hs) 
    {
        if (get_state() != S_HANDSHAKE_WAIT)
            gcomm_throw_fatal << "Invalid state: " << to_string(get_state());
        
        remote_uuid = hs.get_source_uuid();
        
        GMCastMessage hsr (GMCastMessage::T_HANDSHAKE_RESPONSE, 
                           local_uuid, 
                           local_addr,
                           group_name);
        send_msg(hsr);

        set_state(S_HANDSHAKE_RESPONSE_SENT);
    }

    void handle_handshake_response(const GMCastMessage& hs) 
    {
        if (get_state() != S_HANDSHAKE_SENT)
            gcomm_throw_fatal << "Invalid state: " << to_string(get_state());
        
        const string& grp = hs.get_group_name();

        try
        {
            if (grp != group_name)
            {
                log_debug << "Handshake failed, my group: '" << group_name
                          << "', peer group: '" << grp << "'";
                throw false;
            }
            remote_uuid = hs.get_source_uuid();
            URI remote_uri(tp->get_remote_addr());
            remote_addr = remote_uri.get_scheme() + "://" + remote_uri.get_host() + ":"
                + URI(hs.get_node_address()).get_port();

            propagate_remote = true;
            GMCastMessage ok(GMCastMessage::T_HANDSHAKE_OK, local_uuid);
            send_msg(ok);
            set_state(S_OK);
        }
        catch (...)
        {
            log_warn << "Parsing peer address '"
                     << hs.get_node_address() << "' failed.";
            
            GMCastMessage nok (GMCastMessage::T_HANDSHAKE_FAIL, local_uuid);
            
            send_msg (nok);
            set_state(S_FAILED);
        }
    }
    
    void handle_ok(const GMCastMessage& hs) 
    {
        propagate_remote = true;
        set_state(S_OK);
    }
    
    void handle_failed(const GMCastMessage& hs) 
    {
        set_state(S_FAILED);
    }
    

    void handle_topology_change(const GMCastMessage& msg)
    {
        const GMCastMessage::NodeList* nl = msg.get_node_list();
        if (nl == 0)
        {
            log_warn << "null node list";
        }

        UUIDToAddressMap new_map;
        for (GMCastMessage::NodeList::const_iterator i = nl->begin();
             i != nl->end(); ++i)
        {
            if (exists(new_map, i->get_uuid(), i->get_address()))
            {
                log_warn << "Duplicate entry";
                continue;
            }
            new_map.insert(make_pair(i->get_uuid(), i->get_address()));
        }
        
        if (equals(uuid_map, new_map) == false)
        {
            changed = true;
        }
        uuid_map = new_map;
    }

    void send_topology_change(UUIDToAddressMap& um)
    {
        GMCastMessage::NodeList nl;
        for (UUIDToAddressMap::const_iterator i = um.begin(); i != um.end(); ++i)
        {
            if (i->first == UUID() || i->second == string(""))
                gcomm_throw_fatal << "nil uuid or empty address";

            nl.push_back(GMCastNode(true, i->first, i->second));
        }

        GMCastMessage msg(GMCastMessage::T_TOPOLOGY_CHANGE, local_uuid,
                          group_name, nl);
        
        send_msg(msg);
    }

    // @todo: what is this function supposed to do?
    void handle_user(const GMCastMessage& hs)
    {
        if (get_state() != S_OK)
            gcomm_throw_fatal << "invalid state";

        if (hs.get_type() < GMCastMessage::T_USER_BASE)
            gcomm_throw_fatal << "invalid user message";

        gcomm_throw_fatal;
    }
    
    void handle_message(const GMCastMessage& msg) 
    {

        switch (msg.get_type()) {
        case GMCastMessage::T_HANDSHAKE:
            handle_handshake(msg);
            break;
        case GMCastMessage::T_HANDSHAKE_RESPONSE:
            handle_handshake_response(msg);
            break;
        case GMCastMessage::T_HANDSHAKE_OK:
            handle_ok(msg);
            break;
        case GMCastMessage::T_HANDSHAKE_FAIL:
            handle_failed(msg);
            break;
        case GMCastMessage::T_TOPOLOGY_CHANGE:
            handle_topology_change(msg);
            break;
        default:
            handle_user(msg);
        }
    }

    const UUID& get_local_uuid() const
    {
        return local_uuid;
    }

    const UUID& get_remote_uuid() const
    {
        return remote_uuid;
    }

    Transport* get_transport() const
    {
        return tp;
    }

    uint8_t get_send_ttl() const
    {
        return send_ttl;
    }

    void set_send_ttl(const uint8_t t)
    {
        send_ttl = t;
    }


    const string& get_remote_addr() const
    {
        return remote_addr;
    }

    const UUIDToAddressMap& get_uuid_map() const
    {
        return uuid_map;
    }
    

    bool get_changed()
    {
        bool ret = changed;
        changed = false;
        return ret;
    }

    bool get_propagate_remote() const { return propagate_remote; }
    
};

static bool check_uri(const URI& uri)
{
    return (uri.get_scheme() == Conf::TcpScheme);
}

static const string tcp_addr_prefix = Conf::TcpScheme + "://";

GMCast::GMCast(Protonet& net_, const string& uri_)
    :
    Transport     (net_, uri_),
    my_uuid       (0, 0),
    proto_map     (new ProtoMap()),
    spanning_tree (new ProtoMap()),
    listener      (0),
    listen_addr   (tcp_addr_prefix + "0.0.0.0"), // how to make it IPv6 safe?
    initial_addr  (""),
    pending_addrs (),
    remote_addrs  (),
    check_period  ("PT1S"),
    next_check    (Date::now()),
    group_name    ()
{
    if (uri.get_scheme() != Conf::GMCastScheme)
    {
        gcomm_throw_runtime (EINVAL) << "Invalid URL scheme: "
                                     << uri.get_scheme();
    }

    // @todo: technically group name should be in path component
    try
    {
        group_name = uri.get_option (Conf::GMCastQueryGroup);
    }
    catch (gu::NotFound&)
    {
        gcomm_throw_runtime (EINVAL) << "Group not defined in URL: "
                                     << uri.to_string();
    }
    
    try
    {
        if (!host_is_any(uri.get_host()))
        {
            initial_addr = tcp_addr_prefix + uri.get_authority();
            log_debug << "Setting connect address to '" << initial_addr << "'";
        }
    }
    catch (gu::NotSet&)
    {
        //@note: this is different from empty host and indicates URL without ://
        gcomm_throw_runtime (EINVAL) << "Host not defined in URL: "
                                     << uri.to_string();
    }

    try
    {
        listen_addr = uri.get_option (Conf::GMCastQueryListenAddr);
        log_debug << "Setting listen address to " << listen_addr;
    }
    catch (gu::NotFound&) {}

    try
    { 
        gu::URI(listen_addr).get_port();
    }
    catch (gu::NotSet&)
    {
        // if no port is set for listen address in the options,
        // try one from authority part
        try
        {
            listen_addr += ':' + uri.get_port();
        }
        catch (gu::NotSet&)
        {
            listen_addr += ':' + Defaults::Port;
        }
    }

    log_debug << "listening at: " << listen_addr;

    if (check_uri(listen_addr) == false)
    {
        gcomm_throw_runtime (EINVAL) << "Listen addr '" << listen_addr
                                     << "' is not valid";
    }
}

GMCast::~GMCast()
{
    if (listener != 0) stop();
    
    delete proto_map;
    delete spanning_tree;
}

void GMCast::start() 
{    
    log_debug << "gmcast " << get_uuid() << " start";
    URI listen_uri(listen_addr);
    
    set_tcp_defaults (&listen_uri);
    
    listener = Transport::create(get_pnet(), listen_uri.to_string());
    gu_trace (listener->listen());
    gcomm::connect(listener, this, listener->get_fd());

    if (initial_addr != "")
    {
        insert_address(initial_addr, UUID(), pending_addrs);
        gu_trace (gmcast_connect(initial_addr));
    }
    pstack.push_proto(this);
}


void GMCast::stop() 
{
    log_debug << "gmcast " << get_uuid() << " stop";
    pstack.pop_proto(this);
    gcomm_assert(listener != 0);
    gcomm::disconnect(listener, this, listener->get_fd());
    listener->close();
    delete listener;
    listener = 0;    
    spanning_tree->clear();
    
    for (ProtoMap::iterator i = proto_map->begin(); i != proto_map->end(); ++i)
    {
        delete ProtoMap::get_value(i);
    }

    proto_map->clear();
    pending_addrs.clear();
    remote_addrs.clear();
}


void GMCast::gmcast_accept() 
{
    Transport* tp = listener->accept();

    // log_debug << "accepted " << tp->get_fd();
    gcomm::connect(tp, this, tp->get_fd());
    
    GMCastProto* peer = new GMCastProto (tp, listen_addr, "",
                                         get_uuid(), group_name);
    
    pair<ProtoMap::iterator, bool> ret =
        proto_map->insert(make_pair(tp->get_fd(), peer));
    
    if (ret.second == false)
    {
        delete peer;
        gcomm_throw_fatal << "Failed to add peer to map";
    }
    
    peer->send_handshake();
}

void GMCast::gmcast_connect(const string& remote_addr)
{
    if (remote_addr == listen_addr) return;

    // log_debug << "connecting to " << remote_addr;
    
    URI connect_uri(remote_addr);
    
    set_tcp_defaults (&connect_uri);

    Transport* tp = Transport::create(get_pnet(), connect_uri.to_string());

    try 
    {
        tp->connect();
    }
    catch (RuntimeException e)
    {
        log_debug << "Connect failed: " << e.what();
        delete tp;
        return;
    }
    
    gcomm::connect(tp, this, tp->get_fd());

    if (tp->get_state() == Transport::S_CONNECTED)
    {
        // log_debug << "connected " << tp->get_fd();
    }
    else
    {
        // log_debug << "non-blocking connect " << tp->get_fd();
    }

    GMCastProto* peer = new GMCastProto (tp, listen_addr, remote_addr,
                                         get_uuid(), group_name);
    
    pair<ProtoMap::iterator, bool> ret = 
        proto_map->insert(make_pair(tp->get_fd(), peer));
    
    if (ret.second == false)
    {
        delete peer;
        gcomm_throw_fatal << "Failed to add peer to map";
    }
    
    ret.first->second->wait_handshake();
}

void GMCast::gmcast_forget(const UUID& uuid)
{
    /* Close all proto entries corresponding to uuid */
    
    ProtoMap::iterator pi, pi_next;
    for (pi = proto_map->begin(); pi != proto_map->end(); pi = pi_next)
    {
        pi_next = pi, ++pi_next;
        GMCastProto* rp = ProtoMap::get_value(pi);
        if (rp->get_remote_uuid() == uuid)
        {
            gcomm::disconnect(rp->get_transport(), this,
                              rp->get_transport()->get_fd());
            delete rp;
            proto_map->erase(pi);
        }
    }
    
    /* Set all corresponding entries in address list to have retry cnt 
     * max_retry_cnt + 1 and next reconnect time after some period */
    
    AddrList::iterator ai;
    for (ai = remote_addrs.begin(); ai != remote_addrs.end(); ++ai)
    {
        if (get_uuid(ai) == uuid)
        {
            set_retry_cnt(ai, max_retry_cnt + 1);
            set_next_reconnect(ai, Date::now() + Period("PT5S"));
        }
    }
    
    /* Update state */
    update_addresses();
}

void GMCast::handle_connected(GMCastProto* rp)
{
    const Transport* tp = rp->get_transport();
    assert(tp->get_state() == Transport::S_CONNECTED);
    log_debug << "transport " << tp->get_fd() << " connected";
}

void GMCast::handle_established(GMCastProto* rp)
{
    log_debug << self_string() << " connection established to "
              << rp->get_remote_uuid() << " "
              << rp->get_remote_addr();
    
    const string& remote_addr = rp->get_remote_addr();
    AddrList::iterator i = pending_addrs.find (remote_addr);
    
    if (i != pending_addrs.end())
    {
        log_debug << "Erasing " << remote_addr << " from panding list";
        
        pending_addrs.erase(i);
    }
    
    if (rp->get_propagate_remote() == true && 
        (i = remote_addrs.find(remote_addr)) == remote_addrs.end())
    {
        log_debug << "Inserting " << remote_addr << " to remote list";
        
        insert_address (remote_addr, rp->get_remote_uuid(), remote_addrs);
        i = remote_addrs.find(remote_addr);
    }

    // now i points to the address item in remote list

    set_retry_cnt(i, -1);
    update_addresses();
}

void GMCast::handle_failed(GMCastProto* rp)
{
    Transport* tp = rp->get_transport();
    log_debug << tp->get_remote_addr();
    if (tp->get_state() == S_FAILED) 
    {
        // log_debug << "Transport " << tp->get_fd()
        //          << " failed: " << ::strerror(tp->get_errno());
    } 
    else if (tp->get_state() == S_CLOSED)
    {
        // Business as usual
    }
    else
    {
        log_debug << "Transport " << tp->get_fd() 
                  << " in unexpected state " << tp->get_state();
    }

    gcomm::disconnect(tp, this, tp->get_fd());
    
    const string& remote_addr = rp->get_remote_addr();
    
    if (remote_addr != "") // @todo: should this be an assertion?
    {
        AddrList::iterator i;
        
        if ((i = pending_addrs.find(remote_addr)) != pending_addrs.end() ||
            (i = remote_addrs.find(remote_addr))  != remote_addrs.end())
        {
            set_retry_cnt(i, get_retry_cnt(i) + 1);
            
            Date rtime = Date::now() + Period("PT1S");
            
            log_debug << "Setting next reconnect time to "
                      << rtime << " for " << remote_addr;

            set_next_reconnect(i, rtime);
        }
    }
    
    proto_map->erase(rp->get_transport()->get_fd());
    delete rp;
    update_addresses();
}


bool GMCast::is_connected(const string& addr, const UUID& uuid) const
{
    for (ProtoMap::const_iterator i = proto_map->begin();
         i != proto_map->end(); ++i)
    {
        GMCastProto* conn = ProtoMap::get_value(i);

        if (addr == conn->get_remote_addr() || 
            uuid == conn->get_remote_uuid())
            return true;
    }

    return false;
}

void GMCast::insert_address (const string& addr,
                             const UUID&   uuid,
                             AddrList&     alist)
{
    if (addr == listen_addr)
    {
        gcomm_throw_fatal << "Trying to add self to addr list";
    }

    if (alist.insert(make_pair(addr, 
                               Timing(Date::now(),
                                      Date::now(), uuid))).second == false)
    {
        log_warn << "Duplicate entry: " << addr;
    }
    else
    {
        log_debug << self_string() << ": new address entry " << uuid.to_string()
                  << ' ' << addr;
    }
}

using namespace boost;
using std::vector;

typedef adjacency_list< listS, vecS, undirectedS,
                        property<vertex_index_t, UUID>, 
                        property<edge_weight_t,  int> > Graph;

typedef graph_traits <Graph>::edge_descriptor   Edge;
typedef graph_traits <Graph>::vertex_descriptor Vertex;
typedef pair<int, int> E;


static inline int find_safe(const map<const UUID, int>& m, const UUID& val)
{
    map<const UUID, int>::const_iterator i = m.find(val);

    if (i == m.end())
    {
        gcomm_throw_fatal << "Missing UUID " << val.to_string();
    }

    return i->second;
}

static inline const UUID& find_safe(const map<const int, UUID>& m,
                                    const Vertex val)
{
    map<const int, UUID>::const_iterator i = m.find(static_cast<int>(val));

    if (i == m.end()) gcomm_throw_fatal << "Not found: " << val;

    return i->second;
}

void GMCast::compute_spanning_tree(const UUIDToAddressMap& uuid_map)
{
    /* Construct mapping between indexing [0, n) and UUIDs, as well as
     * between UUIDs and proto map entries */
    map<const UUID, int> uuid_to_idx;
    map<const int, UUID> idx_to_uuid;
    map<const UUID, pair<const int, GMCastProto*> > uuid_to_proto;
    
    if (uuid_to_idx.insert(pair<const UUID, int>(get_uuid(), 0)).second ==
        false)
        gcomm_throw_fatal << "Insert to uuid_to_idx failed";

    if (idx_to_uuid.insert(pair<const int, UUID>(0, get_uuid())).second ==
        false)
        gcomm_throw_fatal << "Insert to idx_to_uuid failed";

    int n = 1;

    for (UUIDToAddressMap::const_iterator i = uuid_map.begin();
         i != uuid_map.end(); ++i)
    {
        if (uuid_to_idx.insert(make_pair(i->first, n)).second == true)
        {
            if (idx_to_uuid.insert(make_pair(n, i->first)).second == false)
                gcomm_throw_fatal;
            ++n;
        }
    }
    
    /* Construct lists of edges and weights */
    list<E>   edges;
    list<int> weights;

    for (ProtoMap::const_iterator i = proto_map->begin(); i != proto_map->end();
         ++i)
    {
        const GMCastProto* conn = i->second;
        
        if (conn->get_state() != GMCastProto::S_OK) continue;
        
        uuid_to_proto.insert (make_pair (conn->get_remote_uuid(),
                                         /* make_pair(i->first, i->second) */
                                         *i));
        
        edges.push_back(             // @todo: why not use my_uuid here?
            E(find_safe(uuid_to_idx, conn->get_local_uuid()), 
              find_safe(uuid_to_idx, conn->get_remote_uuid())));
        weights.push_back(1);
        
        for (UUIDToAddressMap::const_iterator j = conn->get_uuid_map().begin();
             j != conn->get_uuid_map().end(); ++j)
        {
            if (j->first != conn->get_local_uuid())
            {
                edges.push_back(
                    E(find_safe(uuid_to_idx, conn->get_remote_uuid()), 
                      find_safe(uuid_to_idx, j->first)));
                weights.push_back(2);
            }
        }
    }
    
    /* Create graph */
    Graph graph(edges.begin(), edges.end(), weights.begin(), n);
 
    /* Compute minimum spanning tree */
    list<Edge> st;

    kruskal_minimum_spanning_tree(graph, std::back_inserter(st));

    /* Reset spanning_tree and proto map states */
    spanning_tree->clear();

    for (ProtoMap::iterator i = proto_map->begin(); i != proto_map->end(); ++i)
    {
        i->second->set_send_ttl(1);
    }
    
    /* Scan through list of edges and construct spanning_tree accordingly, 
     * if source vertex is self. */
    list<Edge>::iterator ei, ei_next;

    for (ei = st.begin(); ei != st.end(); ei = ei_next)
    {
        ei_next = ei, ++ei_next;

        const UUID& source_uuid = find_safe(idx_to_uuid, source(*ei, graph));
        
        if (source_uuid == get_uuid())
        {
            // Edge start vertex is self
            
            map<const UUID, pair<const int, GMCastProto*> >::const_iterator
                up_target = 
                uuid_to_proto.find(find_safe(idx_to_uuid, target(*ei, graph)));
            
            if (up_target != uuid_to_proto.end())
            {
                // std::cerr << up_target->second.first << " ";
                if (spanning_tree->insert(
//                        make_pair(up_target->second.first, 
//                                  up_target->second.second)).second == false)
                        up_target->second).second == false)
                    gcomm_throw_fatal << "Adding connection to spanning tree"
                                      << " failed";
            }

            st.erase(ei);
        }
    }
    
    /* Scan through remaining entries and find out suitable 
     * spanning_tree entry for outgoing route. */
    if (ei != st.end())
    {
        for (ei = st.begin(); ei != st.end(); ++ei)
        {
            log_debug << "multihop route detected, looking up for proper route";
            
            const UUID& source_uuid = find_safe(idx_to_uuid, source(*ei, graph));
            ProtoMap::iterator i;

            for (i = spanning_tree->begin(); i != spanning_tree->end(); ++i)
            {
                if (i->second->get_remote_uuid() == source_uuid)
                {
                    i->second->set_send_ttl(2);
                    break;
                }
            }
            
            if (i == spanning_tree->end())
            {
                log_warn << self_string() << " no outgoing route found for "
                         << source_uuid;
            }
        }
    }
    else
    {
        if (spanning_tree->size() == uuid_to_idx.size() - 1)
        {
            log_debug << self_string() << 
                " all-to-all spanning tree of size " << spanning_tree->size();
        }
        else
        {
            log_debug << self_string()
                      << " spanning tree size "  << spanning_tree->size() 
                      << " number of known nodes " 
                      << uuid_to_idx.size();
        }
    }
}

void GMCast::update_addresses()
{
    UUIDToAddressMap uuid_map;

    /* Add all established connections into uuid_map and update 
     * list of remote addresses */
    for (ProtoMap::iterator i = proto_map->begin(); i != proto_map->end(); ++i)
    {
        GMCastProto* rp = ProtoMap::get_value(i);
        
        if (rp->get_state()            == GMCastProto::S_OK && 
            rp->get_propagate_remote() == true)
        {
            if (rp->get_remote_addr() == "" || rp->get_remote_uuid() == UUID())
            {
                gcomm_throw_fatal << "Protocol error: local: " 
                                  << get_uuid().to_string() << " "
                                  << listen_addr << ", remote: "
                                  << rp->get_remote_uuid().to_string()
                                  << " '" << rp->get_remote_addr() << "'";
            }
            
            if (exists(uuid_map, rp->get_remote_uuid(), 
                       rp->get_remote_addr()) == false)
            {
                uuid_map.insert(make_pair(rp->get_remote_uuid(), 
                                          rp->get_remote_addr()));
            }
            
            if (remote_addrs.find(rp->get_remote_addr()) == remote_addrs.end())
            {
                log_warn << "Connection exists but no addr on addr list for "
                         << rp->get_remote_addr();
                insert_address(rp->get_remote_addr(), rp->get_remote_uuid(), 
                               remote_addrs);
            }
        }
    }
    
    /* Send topology change message containing only established 
     * connections */
    for (ProtoMap::iterator i = proto_map->begin(); i != proto_map->end(); ++i)
    {
        GMCastProto* gp = ProtoMap::get_value(i);
        
        // @todo: a lot of stuff here is done for each connection, including
        //        message creation and serialization. Need a mcast_msg() call
        //        and move this loop in there.
        if (gp->get_state() == GMCastProto::S_OK)
            gp->send_topology_change(uuid_map);
    }
    
    /* Add entries reported by all other nodes to address list to 
     * get complete view of existing uuids/addresses */
    for (ProtoMap::iterator i = proto_map->begin(); i != proto_map->end(); ++i)
    {
        GMCastProto* rp = ProtoMap::get_value(i);
        
        if (rp->get_state() == GMCastProto::S_OK)
        {
            // Add also uuids/addresses that won't be propagated
            if (exists(uuid_map, rp->get_remote_uuid(), 
                       rp->get_remote_addr()) == false)
            {
                uuid_map.insert(make_pair(rp->get_remote_uuid(), 
                                          rp->get_remote_addr()));
            }
            for (UUIDToAddressMap::const_iterator j=rp->get_uuid_map().begin();
                 j != rp->get_uuid_map().end(); ++j)
            {
                gcomm_assert(j->first != UUID::nil() && j->second != "");
                
                if (exists(uuid_map, j->first, j->second) == false)
                {
                    uuid_map.insert(make_pair(j->first, j->second));
                }
                
                if (j->first                      != get_uuid() &&
                    remote_addrs.find(j->second)  == remote_addrs.end() &&
                    pending_addrs.find(j->second) == pending_addrs.end())
                {
                    log_debug << "Conn refers to but no addr in addr list for "
                              << j->second;
                    insert_address(j->second, j->first, pending_addrs);
                    set_retry_cnt(pending_addrs.find(j->second),
                                  max_retry_cnt - 60);
                }
            }
        }
    }
    
    /* Compute spanning tree */
    compute_spanning_tree(uuid_map);
}


void GMCast::reconnect()
{
    /* Loop over known remote addresses and connect if proto entry 
     * does not exist */
    Date now = Date::now();
    AddrList::iterator i, i_next;

    for (i = pending_addrs.begin(); i != pending_addrs.end(); i = i_next)
    {
        i_next = i, ++i_next;

        const string& pending_addr = get_address(i);

        if (is_connected (pending_addr, UUID::nil()) == false)
        {
            if (get_retry_cnt(i) > max_retry_cnt)
            {
                log_debug << "Forgetting " << pending_addr;
                pending_addrs.erase(i);
                continue; // no reference to pending_addr after this
            }
            else if (get_next_reconnect(i) <= now)
            {
                // log_debug << "Connecting to " << pending_addr;
                gmcast_connect (pending_addr);
            }
        }
    }
    
    for (i = remote_addrs.begin(); i != remote_addrs.end(); i = i_next) 
    {
        const UUID& remote_uuid = get_uuid(i);

        if (remote_uuid == get_uuid())
        {
            gcomm_throw_fatal << "Own uuid in remote addr list";
        }

        i_next = i, ++i_next;

        const string& remote_addr = get_address(i);

        if (is_connected (remote_addr, remote_uuid) == false)
        {
            if (get_retry_cnt(i) > max_retry_cnt)
            {
                log_debug << " Forgetting " << remote_uuid.to_string() << " ("
                          << remote_addr << ")";
                remote_addrs.erase(i);
                continue;//no reference to remote_addr or remote_uuid after this
            }
            else if (get_next_reconnect(i) <= now)
            {
                if (get_retry_cnt(i) % 30 == 0)
                {
                    log_info << self_string() << " Reconnecting to " 
                             << remote_uuid.to_string() 
                             << " (" << remote_addr
                             << "), attempt " << get_retry_cnt(i);
                }
                
                gmcast_connect(remote_addr);
            }
            else
            {
                // 
            }
        }
    }
}


Date gcomm::GMCast::handle_timers()
{
    const Date now(Date::now());
    if (now >= next_check)
    {
        reconnect();
        // update_addresses();
        next_check = now + check_period;
    }
    return next_check;
}


void GMCast::forward_message(const int cid, const Datagram& dg, 
                             const GMCastMessage& msg)
{
    gcomm_throw_fatal << "msg ttl dec not implemented";
    
    GMCastMessage send_msg(msg);
    send_msg.dec_ttl();
    assert(send_msg.get_ttl() > 0);
    assert(dg.get_header().size() == 0);
    Datagram send_dg(Buffer(&dg.get_payload()[0] + dg.get_offset(),
                            &dg.get_payload()[0] + dg.get_offset() + 
                            dg.get_len() - msg.serial_size()));
    push_header(send_msg, send_dg);
    for (ProtoMap::iterator i = spanning_tree->begin();
         i != spanning_tree->end(); ++i)
    {
        if (i->first != cid)
        {
            i->second->get_transport()->handle_down(send_dg, ProtoDownMeta());
        }
    }
}


void GMCast::handle_up(int id, const Datagram& dg, const ProtoUpMeta& um) 
{
    ProtoMap::iterator i;

    if (listener == 0)
    {
        return;
    }
    
    if (id == listener->get_fd())
    {
        gmcast_accept();
    }
    else if ((i = proto_map->find(id)) != proto_map->end())
    {
        GMCastProto* p(ProtoMap::get_value(i));
        if (dg.get_len() > 0)
        {
            const GMCastProto::State prev_state(p->get_state());
            GMCastMessage msg;
            msg.unserialize(&dg.get_payload()[0], dg.get_len(), dg.get_offset());
            if (msg.get_type() >= GMCastMessage::T_USER_BASE)
            {
                if (msg.get_ttl() > 1)
                {
                    forward_message(id, dg, msg);
                }

                send_up(Datagram(dg, dg.get_offset() + msg.serial_size()),
                        ProtoUpMeta(msg.get_source_uuid()));
            }
            else
            {
                
                gu_trace(p->handle_message(msg));
                if (p->get_changed() == true)
                {
                    update_addresses();
                }
            }
            if (prev_state     != GMCastProto::S_OK && 
                p->get_state() == GMCastProto::S_OK)
            {
                handle_established(p);
            }
        }
        else if (p->get_transport()->get_state() == Transport::S_CONNECTED &&
                 (p->get_state() == GMCastProto::S_HANDSHAKE_WAIT ||
                  p->get_state() == GMCastProto::S_INIT))
        {
            handle_connected(p);
        }
        else if (p->get_transport()->get_state() == Transport::S_CONNECTED)
        {
            log_warn << "zero len datagram";
        } 
        else
        {
            handle_failed(p);
        }
    }
    else
    {
        // log_debug << "proto entry not found";
    }
}

int GMCast::handle_down(const Datagram& dg, const ProtoDownMeta& dm) 
{

    for (ProtoMap::iterator i = spanning_tree->begin();
         i != spanning_tree->end(); ++i)
    {
        
        GMCastProto* rp = ProtoMap::get_value(i);
        GMCastMessage msg(GMCastMessage::T_USER_BASE, get_uuid(),
                          rp->get_send_ttl());
        Datagram my_dg(dg);
        gu_trace(push_header(msg, my_dg));
        int err;
        if ((err = rp->get_transport()->handle_down(my_dg, 
                                                    ProtoDownMeta())) != 0)
        {
            log_warn << "transport: " << ::strerror(err);
        }
    }
    
    return 0;
}


