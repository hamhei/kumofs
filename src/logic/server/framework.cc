//
// kumofs
//
// Copyright (C) 2009 FURUHASHI Sadayuki
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
//    Unless required by applicable law or agreed to in writing, software
//    distributed under the License is distributed on an "AS IS" BASIS,
//    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//    See the License for the specific language governing permissions and
//    limitations under the License.
//
#include "server/framework.h"

namespace kumo {
namespace server {


std::auto_ptr<framework> net;
std::auto_ptr<resource> share;


void framework::cluster_dispatch(
		shared_node from, weak_responder response,
		rpc::method_id method, rpc::msgobj param, auto_zone z)
try {
	switch(method.get()) {
	RPC_DISPATCH(mod_network, KeepAlive);
	RPC_DISPATCH(mod_network, HashSpaceSync);
	RPC_DISPATCH(mod_store,   ReplicateSet);
	RPC_DISPATCH(mod_store,   ReplicateDelete);
	RPC_DISPATCH(mod_replace, ReplaceCopyStart);
	RPC_DISPATCH(mod_replace, ReplaceDeleteStart);
	RPC_DISPATCH(mod_replace_stream, ReplaceOffer);
	RPC_DISPATCH(mod_control, CreateBackup);
	default:
		throw unknown_method_error();
	}
}
DISPATCH_CATCH(method, response)

void framework::subsystem_dispatch(
		shared_peer from, weak_responder response,
		rpc::method_id method, rpc::msgobj param, auto_zone z)
try {
	switch(method.get()) {
	RPC_DISPATCH(mod_store,   Get);
	RPC_DISPATCH(mod_store,   Set);
	RPC_DISPATCH(mod_store,   Delete);
	RPC_DISPATCH(mod_store,   GetIfModified);
	RPC_DISPATCH(mod_control, GetStatus);
	RPC_DISPATCH(mod_control, SetConfig);
	default:
		throw unknown_method_error();
	}
}
DISPATCH_CATCH(method, response)


void framework::end_preprocess()
{
	mod_replace_stream.stop_stream();
}


void framework::new_node(address addr, role_type id, shared_node n)
{
	// XXX
	LOG_WARN("new node ",(uint16_t)id," ",addr);
	if(addr == share->manager1()) {
		mod_network.renew_r_hash_space();
		mod_network.renew_w_hash_space();
	} else if(share->manager2().connectable() && addr == share->manager2()) {
		mod_network.renew_r_hash_space();
		mod_network.renew_w_hash_space();
	}
}

void framework::lost_node(address addr, role_type id)
{
	// XXX
	LOG_WARN("lost node ",(uint16_t)id," ",addr);
}


}  // namespace server
}  // namespace kumo

