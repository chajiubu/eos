/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 */

#include <eosio/topology_plugin/topology_plugin.hpp>
#include <eosio/topology_plugin/link_descriptor.hpp>
#include <eosio/topology_plugin/node_descriptor.hpp>
#include <eosio/net_plugin/net_plugin.hpp>
#include <eosio/net_plugin/protocol.hpp>
#include <eosio/chain/controller.hpp>
#include <eosio/chain/exceptions.hpp>
#include <eosio/chain/block.hpp>
#include <eosio/chain/plugin_interface.hpp>
#include <eosio/chain/thread_utils.hpp>
#include <eosio/producer_plugin/producer_plugin.hpp>
#include <eosio/chain/contract_types.hpp>
#include <eosio/chain/producer_schedule.hpp>

#include <fc/io/json.hpp>
#include <fc/io/raw.hpp>
#include <fc/log/appender.hpp>
#include <fc/container/flat.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/crypto/rand.hpp>
#include <fc/exception/exception.hpp>

#include <functional>
#include <string>
#include <sstream>
#include <time.h>

//using namespace eosio::chain::plugin_interface::compat;

namespace fc {
   extern std::unordered_map<std::string,logger>& get_logger_map();
}

namespace eosio {
   static appbase::abstract_plugin& _topology_plugin = app().register_plugin<topology_plugin>();

   using std::vector;

   using boost::asio::ip::tcp;
   using boost::asio::ip::address_v4;
   using boost::asio::ip::host_name;
   using boost::multi_index_container;

   using fc::time_point;
   using fc::time_point_sec;
   using eosio::chain::transaction_id_type;
   using eosio::chain::sha256_less;

   /**
    * Link identifies a set of metrics for the network connection between two nodes
    *
    * @attributes
    * id is a unique identifier, a hash of the active and passive node ids
    * active is the id for the "client" connector
    * passive is the id for the "server" connector.
    * hops is a count of intermediate devices, routers, firewalls, etc
    * up holds the metrics bundle for data flow from the "client" to the "server"
    * down holds the metrics bundle for data flow from the "server" to the "client"
    */

   struct topo_link {
      topo_link() : info(), up(), down(), closures(0) {}
      topo_link(link_descriptor &&ld) : up(), down(), closures(0) { info = std::move(ld); }
      link_descriptor info;
      link_metrics   up;
      link_metrics   down;
      uint32_t       closures;
   };

   struct route_descriptor {
      route_descriptor() : length(-1), path(0) {}
      int16_t length;
      link_id path;
   };

   struct topo_node {
      topo_node () : info(), links(), routes() {}
      topo_node (node_descriptor &nd) : links() { info = nd ; }

      node_descriptor   info;
      set<link_id>   links;
      flat_map<node_id,route_descriptor> routes;
      uint32_t last_block_produced;
      link_metrics forks;

      string dot;
      uint64_t primary_key() const { return info.my_id; }

      string dot_label() {
         if (dot.empty()) {
            ostringstream dt;
            dt << info.location << "(" << info.my_id << ")";
            dot = dt.str();
         }
         return dot;
      }
   };

   struct fork_descriptor {
      link_id from_link;
      block_id_type fork_base;
      uint16_t depth;
      uint16_t deficit;
      uint16_t overage;
   };

   struct topo_producer {
      account_name id;
      fork_descriptor current;
      vector<fork_descriptor> forks;
   };

   constexpr auto link_role_str( link_role role ) {
      switch (role) {
      case blocks: return "blocks";
      case transactions: return "transactions";
      case control: return "control";
      case combined: return "combined";
      }
      return "error";
   }

   constexpr auto node_role_str( node_role role ) {
      switch (role) {
      case producer : return "producer";
      case backup : return "backup";
      case api : return "api";
      case full : return "full";
      case gateway : return "gateway";
      case special : return "special";
      }
      return "error";
   }

   constexpr auto metric_str( metric_kind mk ) {
      switch (mk) {
      case queue_depth         : return "queue\\_depth";
      case queue_max_depth     : return "queue\\_max\\_depth";
      case queue_latency       : return "queue\\_latency (us)";
      case net_latency         : return "net\\_latency (us)";
      case bytes_sent          : return "bytes\\_sent";
      case messages_sent       : return "messages\\_sent";
      case bytes_per_second    : return "bytes\\_per\\_second";
      case messages_per_second : return "messages\\_per\\_second";
      case fork_instances      : return "fork\\_instances";
      case fork_depth          : return "fork\\_depth";
      case fork_max_depth      : return "fork\\_max\\_depth";
      }
      return "error";
   }

   const fc::string logger_name("topology_plugin_impl");
   fc::logger topo_logger;

   template<class enum_type, class=typename std::enable_if<std::is_enum<enum_type>::value>::type>
   inline enum_type& operator|=(enum_type& lhs, const enum_type& rhs)
   {
      using T = std::underlying_type_t <enum_type>;
      return lhs = static_cast<enum_type>(static_cast<T>(lhs) | static_cast<T>(rhs));
   }

   constexpr uint16_t def_sample_interval = 5; // seconds betwteen samples
   constexpr uint16_t def_max_hops = 6; // maximum number of times a message can be replicated
   /**
    * Topology plugin implementation details.
    *
    */

   class topology_plugin_impl {
   public:
      flat_map<node_id, topo_node> nodes;
      flat_map<link_id, topo_link> links;
      flat_map<account_name, topo_producer> producers;
      flat_map<metric_kind, std::string> units;

      fc::sha256 gen_long_id( const node_descriptor &desc );
      node_id make_node_id( const fc::sha256 &long_id);
      node_id gen_id( const node_descriptor &desc );
      link_id gen_id( const link_descriptor &desc );
      int16_t find_route_i( set<node_id>& seen, node_id to, topo_node& from );
      int16_t find_route( node_id from, node_id to );
      node_id peer_node( link_id onlink, node_id from );
      node_id add_node( node_descriptor& n );
      void    drop_node( node_id id );
      link_id add_link( link_descriptor&& l );
      void    drop_link( link_id id );

      void    update_samples( const link_sample &ls, bool flip );
      void    update_map( const map_update &mu );
      void    update_forks( const fork_info &fi );
      topo_node *find_node( const chain::name prod );
      void    on_block_recv( link_id src, block_id_type blk_id, const signed_block_ptr sb );
      void    init_topology_message( topology_message &tm );

      uint16_t sample_interval_sec;
      uint16_t max_hops;

      uint16_t max_produced = 12;
      uint16_t block_count = 0;
      account_name prev_producer;

      string bp_name;
      node_id local_node_id;
      bool done = false;
      net_plugin * net_plug = nullptr;
      chain_plugin * chain_plug = nullptr;
      set<chain::account_name> local_producers;

      std::mutex table_mtx;
   };

   void topology_plugin_impl::init_topology_message (topology_message &tm) {
      tm.origin = local_node_id;
      tm.destination = 0;
      tm.fwds = 0;
      tm.ttl = max_hops;
   }

   fc::sha256 topology_plugin_impl::gen_long_id( const node_descriptor &desc ) {
      ostringstream infostrm;
      infostrm << desc.location << desc.role << desc.version;
      if ( !desc.producers.empty() ) {
         for( auto& dp : desc.producers ) {
            infostrm << dp;
         }
      }
      string istr = infostrm.str();
      return fc::sha256::hash(istr.c_str(), istr.size());
   }

   node_id topology_plugin_impl::make_node_id( const fc::sha256 &long_id ) {
      return long_id.data()[0];
   }

   node_id topology_plugin_impl::gen_id( const node_descriptor &desc ) {
      fc::sha256 lid = gen_long_id (desc);
      return make_node_id( lid );
   }

   link_id topology_plugin_impl::gen_id( const link_descriptor &desc ) {
      ostringstream infostrm;
      infostrm << desc.active << desc.passive << link_role_str( desc.role);
      hash<string> hasher;
      return hasher(infostrm.str());
   }

   node_id topology_plugin_impl::add_node( node_descriptor& n ) {
      if (n.my_id == 0) {
         n.my_id = gen_id (n);
      }
      node_id id = n.my_id;
      if (nodes.find(id) == nodes.end()) {
         // ilog(" adding table entry for node, ${id}",("id", id) );

         std::lock_guard<std::mutex> g( table_mtx );

         topo_node tn(n);
         nodes.emplace( id, move(tn));
      }
      // else {
      //    ilog("redundant invocation");
      // }
      return id;
   }

   void topology_plugin_impl::drop_node( node_id id ) {
      nodes.erase( id );
   }

   link_id topology_plugin_impl::add_link( link_descriptor&& l ) {
      auto id = gen_id( l );
      l.my_id = id;
      {
         auto mn = nodes.find(l.active);
         if ( mn != nodes.end()) {
            if (mn->second.links.find ( l.my_id ) == mn->second.links.end()) {
               std::lock_guard<std::mutex> g( table_mtx );
               mn->second.links.insert( l.my_id );
            }
         }
         else {
            ilog ("no node found for active peer ${la}", ("la",l.active));
         }
      }
      {
         auto mn = nodes.find(l.passive);
         if ( mn != nodes.end()) {
            if (mn->second.links.find ( l.my_id ) == mn->second.links.end()) {
               std::lock_guard<std::mutex> g( table_mtx );
               mn->second.links.insert( l.my_id );
            }
         }
         else {
            ilog ("no node found for passive peer ${la}", ("la",l.active));
         }
      }
      {
         std::lock_guard<std::mutex> g( table_mtx );
         links.emplace( id, move(l));
      }
      return id;
   }

   void topology_plugin_impl::drop_link( link_id id ) {
      links[id].closures++;
      //      links.erase( id );
   }

   int16_t topology_plugin_impl::find_route_i( set<node_id>& seen,
                                               node_id to,
                                               topo_node &from ) {
      route_descriptor best = from.routes[to];
      if (best.path != 0) {
         return best.length;
      }
      for( auto lid : from.links ) {
         if(links.find(lid) == links.end()) {
            elog( "link id ${id} not found", ("id",lid));
            continue;
         }
         auto l = links[lid];
         node_id peer = l.info.active == from.info.my_id ? l.info.passive : l.info.active;
         if( peer == to ) {
            best.length = 1;
            best.path = lid;
            from.routes[to] = best;
            return 1;
         }
         if ( seen.find(peer) != seen.end() ) {
            continue;
         }
         seen.insert(peer);
         int16_t res = find_route_i( seen, to, nodes[peer] );
         if( res == -1 ) {
            continue;
         }
         res++;
         if (res < best.length || best.length < 1) {
            best.length = res;
            best.path = lid;
         }
      }
      from.routes[to] = best;
      return best.length;
   }

   int16_t topology_plugin_impl::find_route (node_id from, node_id to) {
      if( nodes.find( to ) == nodes.end() ) {
         elog(" no table entry for target node, ${id}",("id",to) );
         return -1;
      }
      if( nodes.find( from ) == nodes.end() ) {
         elog(" no table entry for source node, ${id}",("id",from) );
         return -1;
      }

      if (to == from) {
         nodes[from].routes[to].length = 0;
         nodes[from].routes[to].path = to;
         return 0;
      }
      else {
         set<node_id> seen;
         seen.insert(from);
         int16_t l =  find_route_i( seen, to, nodes[from] );
         return l;
      }
   }

   node_id topology_plugin_impl::peer_node (link_id onlink, node_id from)
   {
      if (links.find(onlink) == links.end()) {
         wlog("link id ${id} not found",("id",onlink));
         return 0;
      }
      return links[onlink].info.active == from ? links[onlink].info.passive : links[onlink].info.active;
   }

   void topology_plugin_impl::update_forks( const fork_info &fi ) {

   }

   void topology_plugin_impl::update_samples( const link_sample &ls, bool flip) {
      if (links.find(ls.link) != links.end()) {
         links[ls.link].down.sample(flip ? ls.up : ls.down);
         links[ls.link].up.sample(flip ? ls.down : ls.up);
      }
   }

   void topology_plugin_impl::update_map( const map_update &mu ) {
      for( auto &an : mu.add_nodes ) {
         node_descriptor nd = an;
         // ilog("add node with id ${id}",("id",nd.my_id));
         add_node( nd );
      }
      for( auto &al : mu.add_links ) {
         link_descriptor ld = al;
         // ilog( "add link with id ${id}",("id",ld.my_id));
         add_link(move(ld));
      }
      for( auto &dn : mu.drop_nodes ) {
         drop_node(dn);
      }
      for( auto &dl : mu.drop_links ) {
         drop_link(dl);
      }
   }

   topo_node *topology_plugin_impl::find_node( const chain::name prod ) {
      for( auto &l : local_producers ) {
         if( prod == l ) {
            return &nodes[local_node_id];
         }
      }
      for( auto &n : nodes) {
         if( !n.second.info.producers.empty() ) {
            for( auto &p : n.second.info.producers ) {
               if( prod == p ) {
                  cout << "find node found a node for " << prod << endl;
                  return &n.second;
               }
            }
         }
      }
      return nullptr;
   }

   void topology_plugin_impl::on_block_recv(link_id src, block_id_type blk_id, const signed_block_ptr sb) {
      controller &cc = chain_plug->chain();
      block_id_type head = cc.head_block_id();
      try {
         account_name head_prod = cc.head_block_producer();
         account_name pend_prod = cc.pending_block_producer();

         if( sb->producer == head_prod ){
            ++block_count;
            if (block_count > max_produced) {
               uint16_t overage = block_count - max_produced;
               elog( "found producer ${hp} overproduced ${d} blocks", ("hp", head_prod)("d", overage));
            }
         }
         else if( sb->producer == pend_prod ) {
            if (block_count < max_produced) {
               uint16_t deficit = max_produced - block_count;
               elog( "found producer switched to ${pp} from ${hp} ${d} blocks too soon", ("pp", pend_prod)("hp", head_prod)("d",deficit));
               producers[head_prod].forks.emplace_back(fork_descriptor({src,blk_id,block_count,deficit,0}));
               if (producers[prev_producer].current.from_link != 0) {
                  producers[prev_producer].forks.push_back(producers[prev_producer].current);
                  producers[prev_producer].current.from_link = 0;
                  producers[prev_producer].forks.empty();
                  prev_producer = head_prod;
               }
            }
            block_count = 1;
         }
         else if( sb->producer == prev_producer ) {
            elog( "got a block from the previous producer after the switch ", ("pp",prev_producer) );
         }
      }
      catch (const fc::exception &ex) {
         elog ("unable to process block : ${m}", ("m",ex.what()));
      }

   }

   //----------------------------------------------------------------------------------------

   static topology_plugin_impl* my_impl;

   topology_plugin::topology_plugin()
      :my( new topology_plugin_impl ) {
      my_impl = my.get();
   }

   topology_plugin::~topology_plugin() {
   }

   void topology_plugin::set_program_options( options_description& /*cli*/, options_description& cfg )
   {
      cfg.add_options()
         ( "bp-name", bpo::value<string>(), "\"block producer name\" but really any identifier that localizes a set of nodeos hosts" )
         ( "sample-interval-seconds", bpo::value<uint16_t>()->default_value(def_sample_interval), "delay between samples")
         ( "max-hops", bpo::value<uint16_t>()->default_value(def_max_hops), "maximum number of times a given message is repeated when distributing" )
         ;
   }

   template<typename T>
   T dejsonify(const string& s) {
      return fc::json::from_string(s).as<T>();
   }

   void topology_plugin::plugin_initialize( const variables_map& options ) {
      EOS_ASSERT( options.count( "bp-name" ) > 0, chain::plugin_config_exception,
                  "the topology module requires a bp-name be supplied" );
      try {
         my->bp_name = options.at( "bp-name" ).as<string>();
         my->sample_interval_sec = options.at( "sample-interval-seconds").as<uint16_t>();
         EOS_ASSERT( my->sample_interval_sec > 0, chain::plugin_config_exception, "sampling frequency must be greater than zero.");
         my->max_hops = options.at( "max-hops" ).as<uint16_t>();

      }
      catch ( const fc::exception &ex) {
      }
      if( options.count( "producer-name" ) ) {
         const std::vector<std::string>& ops = options["producer-name"].as<std::vector<std::string>>();
         for( const auto& v : ops ) {
            my->local_producers.emplace( eosio::chain::name( v ) );
         }
      }
   }

   void topology_plugin::plugin_startup() {
      my->net_plug = app().find_plugin<net_plugin>();
      EOS_ASSERT( my->net_plug != nullptr, chain::plugin_config_exception, "No net plugin found.");
      my->chain_plug = app().find_plugin<chain_plugin>();

   }

   void topology_plugin::plugin_shutdown() {
      try {
         my->done = true;
      }
      FC_CAPTURE_AND_RETHROW()
   }


   const string& topology_plugin::bp_name () {
      return my->bp_name;
   }

   fc::sha256 topology_plugin::gen_long_id( const node_descriptor &desc ) {
      return my->gen_long_id(desc);
   }

   node_id topology_plugin::make_node_id( const fc::sha256 &long_id ) {
      return my->make_node_id(long_id);
   }

   void topology_plugin::set_local_node_id( node_id id) {
      my->local_node_id = id;
   }

   node_id topology_plugin::add_node( node_descriptor& n, map_update *mu ) {
      if( mu != nullptr ) {
         mu->add_nodes.push_back( n );
      }
      return my->add_node( n);
   }

   void  topology_plugin::drop_node( node_id id, map_update *mu ) {
      if( mu != nullptr ) {
         mu->drop_nodes.push_back( id );
      }
      my->drop_node( id );
   }

   link_id topology_plugin::add_link( link_descriptor&& l, map_update *mu ) {
      if( mu != nullptr ) {
         mu->add_links.push_back( l );
      }
      return my->add_link(move(l));
   }

   void topology_plugin::drop_link( link_id id, map_update *mu ) {
      if( mu != nullptr ) {
         mu->drop_links.push_back( id );
      }
      my->drop_link(id);
   }

   string topology_plugin::nodes( const string& in_roles ) {
      //convert in_roles to list of node roles then add them up
      vector<node_role> roles;
      uint64_t nr = 0;
      for (auto &pr : roles) {
         nr |= pr;
      }
      bool any = nr == 0;
      vector< node_descriptor > specific_nodes;
      for (auto &n : my->nodes) {
         const node_descriptor &nd(n.second.info);
         if ( any || ((nr & nd.role) != 0 ) )
            specific_nodes.push_back(nd);
      }
      ostringstream res;
      res << fc::json::to_pretty_string( specific_nodes ) << ends;

      return res.str();
   }

   void topology_plugin::init_node_descriptor(node_descriptor& nd,
                                              const fc::sha256& id,
                                              const string& address,
                                              const string& version) {
      nd.my_id = make_node_id(id);
      nd.location = bp_name() + ":" + address;
      nd.role = producer;
      nd.status = running;
      stringstream ss;
      ss << version;
      nd.version = ss.str();
      for (auto &lp : my->local_producers) {
         nd.producers.emplace_back(lp);
      }
   }

   string topology_plugin::links( const string&  with_node ) {
      node_descriptor nd; // = unpack with_node
      node_id id = my->gen_id(nd);
      vector< link_descriptor > specific_links;

      for (auto &l : my->links) {
         const link_descriptor &ld(l.second.info);
         if ( ld.active == id || ld.passive == id )
            specific_links.push_back(ld);
      }
      ostringstream res;
      res << fc::json::to_pretty_string( specific_links ) << ends;

      return res.str();
   }

   void topology_plugin::send_updates( const topology_data &td) {
      topology_message tm;
      tm.origin = my->local_node_id;
      tm.destination = 0;
      tm.ttl = 1; // my->max_hops;
      if (td.contains<link_sample>() ) {
         const link_sample &ls = td.get<link_sample>();
         my->update_samples( ls, false );
         tm.destination = my->peer_node(ls.link, tm.origin);
         tm.ttl = 1;
         ilog("sending new link sample message");
      }
      else if (td.contains<map_update>() ) {
         ilog("sending a map update message");
      }
      tm.fwds = 0;
      tm.payload.push_back( td );
      topo_update(tm);
   }

   uint16_t topology_plugin::sample_interval_sec () {
      return my->sample_interval_sec;
   }

   class topo_msg_handler : public fc::visitor<void> {
   public:
      void operator()( const map_update& msg) {
         ilog("got a map update message");
         my_impl->update_map( msg );
      }

      void operator()( const link_sample& msg) {
         ilog("got a link sample message");
         my_impl->update_samples( msg, true );
      }

      void operator()( const fork_info& msg) {
         ilog("got a fork info message");
         my_impl->update_forks( msg );
      }
   };

   void topology_plugin::on_block_recv( link_id src, block_id_type blk_id, const signed_block_ptr msg ) {
      my->on_block_recv( src, blk_id, msg );
   }

   void topology_plugin::handle_message( const topology_message& msg )
   {
      dlog("handling a new topology message");
      //      topology_message_ptr ptr = std::make_shared<topology_message>( msg );
      for ( const auto &p : msg.payload) {
         topo_msg_handler tm;
         p.visit( tm );
      }
      msg.fwds++;
      if( msg.ttl > msg.fwds ) {
         dlog("forwarding topology message. ttl = ${t}. fwds = ${f}",("t",msg.ttl)("f",msg.fwds));
         topo_update(msg);
      }
   }

   // deciding whether or not to forward depends on:
   // 1. did this already come from us?
   // 2. are we on the shortest path?
   // 3. is the forward count equal to our number of hops from the origin?
   bool topology_plugin::forward_topology_message(const topology_message& tm, link_id li ) {
      auto &link = my->links[li];
      node_id peerid = link.info.active == my->local_node_id ? link.info.passive : link.info.active;
      elog ("origin = ${id} dest = ${to}, forwards = ${f}, ttl = ${tt}", ("id",tm.origin)("f",tm.fwds)("tt",tm.ttl)("to",tm.destination));
      if( tm.origin == my->local_node_id && tm.fwds > 0 ) {
         return false;
      }
      auto len = my->nodes[tm.origin].routes[my->local_node_id].length;
      if (len == -1) {
         len = my->find_route(my->local_node_id, tm.origin);
      }
      if (len < tm.fwds) {
         elog ("message has too many hops, distance = ${d}, ttl = ${t}",
                  ("d",len)("t",tm.fwds));
         return false;
      }

      return true;
   }


   string topology_plugin::grid( ) {
      ostringstream df;
      df << " digraph G\n{\nlayout=\"circo\";" << endl;
      set<link_id> seen;
      for (auto &tnode : my->nodes) {
         for (const auto &l : tnode.second.links) {
            if (seen.find(l) != seen.end()) {
               ilog("link id ${id} already seen",("id",l));
               continue;
            }
            auto tlink = my->links.find(l);
            if ( tlink == my->links.end() ) {
               ilog( "did not find link id ${id}", ("id",l));
               continue;
            }
            seen.insert(tlink->second.info.my_id);
            string alabel, plabel;
            if (tlink->second.info.passive == tnode.first) {
               alabel = my->nodes[tlink->second.info.active].dot_label();
               plabel = tnode.second.dot_label();
            } else {
               plabel = my->nodes[tlink->second.info.active].dot_label();
               alabel = tnode.second.dot_label();
            }

            df << "\"" << alabel << "\""
               << " -> " << "\"" << plabel << "\""
               << " [dir=\"forward\"];" << endl;
         }
      }
      df << "}\n";

      return df.str();

   }

   string topology_plugin::sample( ) {
      ostringstream df;
      df << "{ \"links\" = [" << endl;
      for (auto &tlink : my->links) {
         df << "{ \"key\" = \"" << tlink.first << "\"," << endl;
         df << "\"value\" = " << fc::json::to_pretty_string( tlink.second ) << "}" << endl;
      }
      df << "]}" << endl;

      return df.str();

   }

   string topology_plugin::report( ) {
      ostringstream df;
      time_t now;
      time(&now);
      struct tm stm;
      gmtime_r(&now, &stm);
      char tmbuf[100];

      df << "# Link Performance Metrics\ngenerated " << asctime_r(&stm, tmbuf);
      auto tnode = my->nodes[my->local_node_id];
      df << "<br>reporting node: " << tnode.info.location << "\n";

      df << "\n# Active Producer List\n";
      if( !my->local_producers.empty() ) {
         df << "## Local Producers\n";
         for (auto &lp : my->local_producers) {
            df << lp << "\n";
         }
      }
      controller &db = my->chain_plug->chain();

      df << "total nodes " << my->nodes.size() << " \n";


      auto &plist = db.active_producers().producers;
      if( plist.empty() ) {
         df << "\n cannot retrieve active producers list \n";
      }
      else {
         size_t pcount = plist.size();
         df << "\nschedule has " << pcount << " producers\n";

         topo_node *pnode = my->find_node(plist[pcount - 1].producer_name);
         if (pnode == nullptr) {
            df << "\n cannot resolve producer " << plist[pcount-1].producer_name << "\n";
         }
         else {
            df << "\n| Producer Account | Location |     Id      | Hops |\n";
            df <<   "|------------------|----------|-------------|------|\n";
            node_id prev_node_id = pnode->info.my_id;
            for( const auto &ap : plist) {
               pnode = my->find_node(ap.producer_name);
               if (pnode == nullptr ) {
                  df << "\n cannot resolve producer " << ap.producer_name << "\n";
                  break;
               }
               df << ap.producer_name << " | ";
               if (pnode->info.location.empty() ) {
                  df << "unknown | ";
               }
               else {
                  df << pnode->info.location.c_str() << " | ";
               }

               df << pnode->info.my_id << " | ";

               auto len = pnode->routes[prev_node_id].length;
               if ( pnode->routes[prev_node_id].path == 0 ) {
                  len = my->find_route(prev_node_id, pnode->info.my_id);
               }
               df << len;
               if( my->producers.find(ap.producer_name) == my->producers.cend()) {
                  df << 0;
               }
               else {
                  df << my->producers[ap.producer_name].forks.size();
               }
               df << "\n";
               prev_node_id = pnode->info.my_id;
            }
         }
      }

      df << "\nNumber of producers indicating microforks: " << my->producers.size() << "\n";
      if ( my->producers.size() ) {
         for (auto &tprod : my->producers) {
            df << "\nProducer " << tprod.first << " has " << tprod.second.forks.size() << " episodes reported\n";
            for (auto &fdes : tprod.second.forks) {
               df << " from link " << fdes.from_link << " symptom ";
               if (fdes.depth > 0) {
                  df << " fork of " << fdes.depth << " blocks ";
               }
               else if (fdes.deficit > 0) {
                  df << " production deficit of " << fdes.deficit << " blocks ";
               }
               else if (fdes.overage > 0) {
                  df << " produced " << fdes.overage << " too many blocks ";
               }
               else {
                  df << " reporting failure, no fork symptom recorded ";
               }
               df << "\n";
            }
         }
      }

      int tn = 1;
      int anon = 0;
      for (auto &tlink : my->links) {
         if ( my->nodes[tlink.second.info.active].info.location.empty() ) {
            ++anon;
            continue;
         }
         df << "\n## Link " << tn++ << "\n";
         df << "active connector: "
            << my->nodes[tlink.second.info.active].info.location << "\n";
         df << "<br>passive connector: "
            << my->nodes[tlink.second.info.passive].info.location << "\n";
         if ( tlink.second.closures > 0) {
            df << "<br>closure count: " << tlink.second.closures << "\n";
         }
         df << "### Measurements from passive to active\n";
         if (tlink.second.down.last_sample != 0) {
            now = tlink.second.down.last_sample;
            gmtime_r( &now, &stm);
            df << "last sample time " << asctime_r(&stm, tmbuf);
            now = tlink.second.down.first_sample;
            gmtime_r( &now, &stm);
            df << "<br>first sample time " << asctime_r(&stm, tmbuf);
            df << "<br>total bytes " << tlink.second.down.total_bytes << "\n";
            df << "<br>total messages " << tlink.second.down.total_messages << "\n\n";
            df << "| metric name | sample count | last reading | min value | avg value | max value |\n";
            df << "|-------------|--------------|--------------|-----------|-----------|-----------|\n";
            int lines = 0;
            for( auto &met : tlink.second.down.measurements ) {
               df << metric_str(met.first) << " | "
                  << met.second.count << " | "
                  << met.second.last << " | "
                  << met.second.min << " | "
                  << met.second.avg << " | "
                  << met.second.max << "\n";
               ++lines;
            }
            lines = 0;
            df << "\n### Measurements from active to passive\n";
            now = tlink.second.up.last_sample;
            gmtime_r( &now, &stm);
            df << "last sample time " << asctime_r(&stm, tmbuf);
            now = tlink.second.up.first_sample;
            gmtime_r( &now, &stm);
            df << "<br>first sample time " << asctime_r(&stm, tmbuf);
            df << "<br>total bytes " << tlink.second.up.total_bytes << "\n";
            df << "<br>total messages " << tlink.second.up.total_messages << "\n\n";
            df << "| metric name | sample count | last reading | min value | avg value | max value |\n";
            df << "|-------------|--------------|--------------|-----------|-----------|-----------|\n";
            for( auto &met : tlink.second.up.measurements ) {
               df << metric_str(met.first) << " | "
                  << met.second.count << " | "
                  << met.second.last << " | "
                  << met.second.min << " | "
                  << met.second.avg << " | "
                  << met.second.max << "\n";
               ++lines;
            }
         }
         else {
            df << "\nno measurements available\n";
         }
      }
      if (anon > 0) {
         df << "\n skipped " << anon << " anonymous links\n";
      }
      return df.str();
   }
}

FC_REFLECT(eosio::topo_link,(info)(up)(down)(closures))
FC_REFLECT(eosio::route_descriptor,(length)(path))
FC_REFLECT(eosio::topo_node,(info)(links)(routes))
