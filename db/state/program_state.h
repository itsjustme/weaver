/*
 * ===============================================================
 *    Description:  State corresponding to a node program.
 *
 *        Created:  04/23/2013 10:44:00 AM
 *
 *         Author:  Ayush Dubey, dubey@cs.cornell.edu
 *
 * Copyright (C) 2013, Cornell University, see the LICENSE file
 *                     for licensing agreement
 * ===============================================================
 */

#ifndef __PROG_STATE__
#define __PROG_STATE__

#include <unordered_map>
#include <unordered_set>
#include <po6/threads/mutex.h>

#include "node_prog/node_prog_type.h"
#include "node_prog/reach_program.h"
#include "node_prog/dijkstra_program.h"
#include "node_prog/clustering_program.h"
#include "common/message.h"

namespace state
{
    typedef std::unordered_map<uint64_t, node_prog::Packable_Deletable*> req_map;
    typedef std::unordered_map<uint64_t, req_map> node_map;
    typedef std::unordered_map<node_prog::prog_type, node_map> prog_map;
    // for permanent deletion
    typedef std::unordered_map<uint64_t, std::pair<uint32_t, std::unordered_set<uint64_t>>> req_to_nodes;

    class program_state
    {
        prog_map prog_state;
        req_to_nodes node_list;
        uint64_t completed_id; // all nodes progs with id < completed_id are done
        std::unordered_set<uint64_t> done_ids; // TODO clean up of done_ids
        po6::threads::mutex mutex;
        bool holding;
        po6::threads::cond in_use_cond;

        public:
            program_state();

        public:
            bool state_exists(node_prog::prog_type t, uint64_t req_id, uint64_t node_handle);
            node_prog::Packable_Deletable* get_state(node_prog::prog_type t,
                uint64_t req_id, uint64_t node_handle);
            void put_state(node_prog::prog_type t, uint64_t req_id, uint64_t node_handle,
                node_prog::Packable_Deletable *new_state);
            uint64_t size(uint64_t node_handle);
            void pack(uint64_t node_handle, e::buffer::packer &packer);
            void unpack(uint64_t node_handle, e::unpacker &unpacker);
            void delete_node_state(uint64_t node_handle);
            void done_requests(std::vector<std::pair<uint64_t, node_prog::prog_type>>&, uint64_t max_done_id);
            bool check_done_request(uint64_t req_id);
            void clear_in_use(uint64_t req_id);

        private:
            void acquire();
            void release();
            bool state_exists_nolock(node_prog::prog_type t, uint64_t req_id, uint64_t node_handle);
            bool node_entry_exists_nolock(node_prog::prog_type t, uint64_t node_handle);
    };

    program_state :: program_state()
        : completed_id(0)
        , holding(false)
        , in_use_cond(&mutex)
    {
        node_map new_node_map;
        prog_state.emplace(node_prog::REACHABILITY, new_node_map);
        prog_state.emplace(node_prog::DIJKSTRA, new_node_map);
        prog_state.emplace(node_prog::CLUSTERING, new_node_map);
    }

    inline void
    program_state :: acquire()
    {
        mutex.lock();
        holding = true;
    }

    inline void
    program_state :: release()
    {
        holding = false;
        mutex.unlock();
    }

    inline bool
    program_state :: state_exists_nolock(node_prog::prog_type t, uint64_t req_id, uint64_t node_handle)
    {
        node_map &nmap = prog_state.at(t);
        node_map::iterator nmap_iter = nmap.find(node_handle);
        if (nmap_iter == nmap.end()) {
            return false;
        }
        req_map &rmap = nmap.at(node_handle);
        req_map::iterator rmap_iter = rmap.find(req_id);
        if (rmap_iter == rmap.end()) {
            return false;
        } else {
            return true;
        }
    }
    
    inline bool
    program_state :: node_entry_exists_nolock(node_prog::prog_type t, uint64_t node_handle)
    {
        node_map &nmap = prog_state.at(t);
        node_map::iterator nmap_iter = nmap.find(node_handle);
        if (nmap_iter == nmap.end()) {
            return false;
        } else {
            return true;
        }
    }

    inline bool
    program_state :: state_exists(node_prog::prog_type t, uint64_t req_id, uint64_t node_handle)
    {
        bool exists;
        acquire();
        exists = state_exists_nolock(t, req_id, node_handle);
        release();
        return exists;
    }

    inline node_prog::Packable_Deletable* 
    program_state :: get_state(node_prog::prog_type t, uint64_t req_id, uint64_t node_handle)
    {
        node_prog::Packable_Deletable *state = NULL;
        acquire();
        if (state_exists_nolock(t, req_id, node_handle)) {
            state = prog_state.at(t).at(node_handle).at(req_id);
        }
        release();
        return state;
    }

    inline void
    program_state :: put_state(node_prog::prog_type t, uint64_t req_id, uint64_t node_handle,
        node_prog::Packable_Deletable *new_state)
    {
        acquire();
        if (state_exists_nolock(t, req_id, node_handle)) {
            node_prog::Packable_Deletable *old_state = prog_state.at(t).at(node_handle).at(req_id);
            delete old_state;
        } else {
            node_list.at(req_id).second.emplace(node_handle);
        }
        prog_state[t][node_handle][req_id] = new_state;
        release();
    }
    
    inline uint64_t
    program_state :: size(uint64_t node)
    {
        uint64_t sz = 0;
        uint16_t ptype;
        acquire();
        for (auto &t: prog_state) {
            sz += message::size(ptype);
            sz += sizeof(uint64_t);
            if (node_entry_exists_nolock(t.first, node)) {
                //sz += message::size(t.second.at(node));
                for (const std::pair<uint64_t, node_prog::Packable_Deletable*> &r: t.second.at(node)) {
                    sz += message::size(r.first);
                    switch (t.first)
                    {
                        case node_prog::REACHABILITY: {
                            node_prog::reach_node_state *rns = 
                                dynamic_cast<node_prog::reach_node_state*>(r.second);
                            sz += rns->size();
                            break;
                        }

                        case node_prog::DIJKSTRA: {
                            node_prog::dijkstra_node_state *dns =
                                dynamic_cast<node_prog::dijkstra_node_state*>(r.second);
                            sz += dns->size();
                            break;
                        }

                        case node_prog::CLUSTERING: {
                            node_prog::clustering_node_state *cns =
                                dynamic_cast<node_prog::clustering_node_state*>(r.second);
                            sz += cns->size();
                            break;
                        }

                        default:
                            DEBUG << "Bad type in program state size " << t.first << std::endl;
                    }
                }
            }
        }
        release();
        return sz;
    }

    inline void
    program_state :: pack(uint64_t node, e::buffer::packer &packer)
    {
        uint16_t ptype;
        uint64_t size = 0;
        acquire();
        for (auto const &t: prog_state) {
            ptype = (uint16_t)t.first;
            message::pack_buffer(packer, ptype);
            if (node_entry_exists_nolock(t.first, node)) {
                size = t.second.at(node).size();
                message::pack_buffer(packer, size);
                for (const std::pair<uint64_t, node_prog::Packable_Deletable*> &r: t.second.at(node)) {
                    message::pack_buffer(packer, r.first);
                    switch (t.first)
                    {
                        case node_prog::REACHABILITY: {
                            node_prog::reach_node_state *rns =
                                dynamic_cast<node_prog::reach_node_state*>(r.second);
                            rns->pack(packer);
                            break;
                        }

                        case node_prog::DIJKSTRA: {
                            node_prog::dijkstra_node_state *dns =
                                dynamic_cast<node_prog::dijkstra_node_state*>(r.second);
                            dns->pack(packer);
                            break;
                        }

                        case node_prog::CLUSTERING: {
                            node_prog::clustering_node_state *cns =
                                dynamic_cast<node_prog::clustering_node_state*>(r.second);
                            cns->pack(packer);
                            break;
                        }

                        default:
                            DEBUG << "Bad type in program state pack " << t.first << std::endl;
                    }
                }
            } else {
                size = 0;
                message::pack_buffer(packer, size);
            }
        }
        release();
    }

    inline void
    program_state :: unpack(uint64_t node, e::unpacker &unpacker)
    {
        uint16_t ptype;
        node_prog::prog_type type;
        acquire();
        for (auto &t: prog_state) {
            req_map rmap;
            node_prog::Packable_Deletable *new_entry;
            message::unpack_buffer(unpacker, ptype);
            type = (node_prog::prog_type)ptype;
            // unpacking map now
            uint64_t elements_left;
            uint64_t key_to_add;
            unpacker = unpacker >> elements_left;
            // set number of buckets to 1.25*elements it will contain
            // did not use reserve as max_load_factor is default 1
            rmap.rehash(elements_left*1.25); 

            while (elements_left > 0) {
                message::unpack_buffer(unpacker, key_to_add);
                switch (t.first)
                {
                    case node_prog::REACHABILITY: {
                        node_prog::reach_node_state *rns = new node_prog::reach_node_state();
                        rns->unpack(unpacker);
                        new_entry = dynamic_cast<node_prog::Packable_Deletable*>(rns);
                        break;
                    }

                    case node_prog::DIJKSTRA: {
                        node_prog::dijkstra_node_state *dns = new node_prog::dijkstra_node_state();
                        dns->unpack(unpacker);
                        new_entry = dynamic_cast<node_prog::Packable_Deletable*>(dns);
                        break;
                    }

                    case node_prog::CLUSTERING: {
                        node_prog::clustering_node_state *cns = new node_prog::clustering_node_state();
                        cns->unpack(unpacker);
                        new_entry = dynamic_cast<node_prog::Packable_Deletable*>(cns);
                        break;
                    }

                    default:
                        DEBUG << "Bad type in program state unpack " << t.first << std::endl;
                }
                if (!rmap.emplace(key_to_add, new_entry).second) {
                    DEBUG << "Insertion unsuccessful in state" << std::endl;
                }
                if (node_list.find(key_to_add) == node_list.end()) {
                    node_list.emplace(key_to_add, std::make_pair(0, std::unordered_set<uint64_t>()));
                }
                node_list.at(key_to_add).second.emplace(node);
                elements_left--;
            }

            if (rmap.size() > 0) {
                if (!prog_state.at(type).emplace(node, rmap).second) {
                    DEBUG << "Bad insertion in prog_state map for node " << node << std::endl;
                }
            }
        }
        release();
    }

    inline void
    program_state :: delete_node_state(uint64_t node_handle)
    {
        acquire();
        for (auto &t: prog_state) {
            if (node_entry_exists_nolock(t.first, node_handle)) {
                for (const std::pair<uint64_t, node_prog::Packable_Deletable*> &r: t.second.at(node_handle)) {
                    delete r.second;
                    node_list.at(r.first).second.erase(node_handle);
                }
                t.second.erase(node_handle);
            }
        }
        release();
    }

    inline void
    program_state :: done_requests(std::vector<std::pair<uint64_t, node_prog::prog_type>> &reqs, uint64_t max_done_id)
    {
        UNUSED(max_done_id);
        acquire();
        //if (max_done_id < completed_id) {
        //    DEBUG << "Max done id " << max_done_id << ", completed id " << completed_id << std::endl;
        //}
        //assert(max_done_id >= completed_id);
        //completed_id = max_done_id;
        for (auto &p: reqs) {
            uint64_t req_id = p.first;
            node_prog::prog_type type = p.second;
            done_ids.emplace(req_id);
            if (node_list.find(req_id) == node_list.end()) {
                continue;
            }
            while (node_list.at(req_id).first > 0) {
                in_use_cond.wait();
            }
            std::unordered_set<uint64_t> &nodes = node_list.at(req_id).second;
            node_map &nmap = prog_state.at(type);
            for (uint64_t n: nodes) {
                delete nmap.at(n).at(req_id);
                nmap.at(n).erase(req_id);
                if (nmap.at(n).empty()) {
                    nmap.erase(n);
                }
            }
            node_list.erase(req_id);
        }
        release();
    }
            
    inline bool
    program_state :: check_done_request(uint64_t req_id)
    {
        bool ret;
        acquire();
        //ret = (req_id < completed_id);
        ret = (done_ids.find(req_id) != done_ids.end());
        if (!ret) {
            // increment in use counter to prevent deletion
            if (node_list.find(req_id) == node_list.end()) {
                node_list.emplace(req_id, std::make_pair(0, std::unordered_set<uint64_t>()));
            }
            node_list.at(req_id).first++;
        }
        release();
        return ret;
    }

    inline void
    program_state :: clear_in_use(uint64_t req_id)
    {
        acquire();
        node_list.at(req_id).first--;
        in_use_cond.broadcast();
        release();
    }
}

#endif
