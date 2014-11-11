/*
 * ===============================================================
 *    Description:  Vector timestamper server loop and request
 *                  processing methods.
 *
 *        Created:  07/22/2013 02:42:28 PM
 *
 *         Author:  Ayush Dubey, dubey@cs.cornell.edu
 *
 * Copyright (C) 2013, Cornell University, see the LICENSE file
 *                     for licensing agreement
 * ===============================================================
 */

#include <iostream>
#include <thread>
#include <vector>
#include <deque>
#include <stdlib.h>
#include <signal.h>
#include <sys/time.h>
#include <e/popt.h>

#define weaver_debug_
#include "common/vclock.h"
#include "common/transaction.h"
#include "common/event_order.h"
#include "common/config_constants.h"
#include "common/bool_vector.h"
#include "node_prog/node_prog_type.h"
#include "node_prog/node_program.h"
#include "coordinator/timestamper.h"

DECLARE_CONFIG_CONSTANTS;

using coordinator::current_prog;
using coordinator::blocked_prog;
using transaction::done_req_t;
static coordinator::timestamper *vts;
static uint64_t vt_id;

// tx functions
void prepare_tx(transaction::pending_tx *tx, coordinator::hyper_stub *hstub, order::oracle *time_oracle);
void end_tx(uint64_t tx_id, coordinator::hyper_stub *hstub, uint64_t shard_id);


void
end_program(int signum)
{
    std::cerr << "Ending program, signum = " << signum << std::endl;
    vts->clk_rw_mtx.wrlock();
    WDEBUG << "num vclk updates " << vts->clk_updates << std::endl;
    vts->clk_rw_mtx.unlock();

#ifdef weaver_benchmark_
    WDEBUG << "max outstanding prog cnt = " << vts->max_outstanding_cnt << std::endl;
#endif

    if (signum == SIGINT) {
        // TODO proper shutdown
        //vts->exit_mutex.lock();
        //vts->to_exit = true;
        //vts->exit_mutex.unlock();
        exit(0);
    } else {
        WDEBUG << "Got interrupt signal other than SIGINT, exiting immediately." << std::endl;
        exit(0);
    }
}

void
prepare_tx(transaction::pending_tx *tx, coordinator::hyper_stub *hstub, order::oracle *time_oracle)
{
    //tx->id = vts->generate_req_id();

    std::unordered_set<node_handle_t> get_set;
    std::unordered_set<node_handle_t> del_set;
    std::unordered_map<node_handle_t, uint64_t> put_map;
    std::unordered_map<node_handle_t, uint64_t>::iterator find_iter; 

    for (transaction::pending_update *upd: tx->writes) {
        switch (upd->type) {

            case transaction::NODE_CREATE_REQ:
                // randomly assign shard for this node
                upd->loc1 = vts->generate_loc(); // node will be placed on this shard
                put_map.emplace(upd->handle, upd->loc1);
                WDEBUG << "put map has " << upd->handle << " -> " << upd->loc1 << std::endl;
                break;

            case transaction::EDGE_CREATE_REQ:
                if ((find_iter = put_map.find(upd->handle1)) == put_map.end()) {
                    upd->loc1 = UINT64_MAX;
                    get_set.insert(upd->handle1);
                } else {
                    upd->loc1 = find_iter->second;
                }
                if ((find_iter = put_map.find(upd->handle2)) == put_map.end()) {
                    upd->loc2 = UINT64_MAX;
                    get_set.insert(upd->handle2);
                } else {
                    upd->loc2 = find_iter->second;
                }
                break;

            case transaction::NODE_DELETE_REQ:
            case transaction::NODE_SET_PROPERTY:
                if ((find_iter = put_map.find(upd->handle1)) == put_map.end()) {
                    upd->loc1 = UINT64_MAX;
                    get_set.insert(upd->handle1);
                } else {
                    upd->loc1 = find_iter->second;
                }
                upd->loc2 = 0;
                if (upd->type == transaction::NODE_DELETE_REQ) {
                    del_set.emplace(upd->handle1);
                }
                break;

            case transaction::EDGE_DELETE_REQ:
            case transaction::EDGE_SET_PROPERTY:
                if ((find_iter = put_map.find(upd->handle2)) == put_map.end()) {
                    upd->loc1 = UINT64_MAX;
                    get_set.insert(upd->handle2);
                } else {
                    upd->loc1 = find_iter->second;
                }
                upd->loc2 = 0;
                break;

            default:
                WDEBUG << "bad type" << std::endl;
        }
    }

    uint64_t sender = tx->sender;
    bool ready = false;
    bool error = false;

    while (!ready && !error) {
        vts->clk_rw_mtx.wrlock();
        vts->vclk.increment_clock();
        vts->out_queue_counter++;
        tx->timestamp = vts->vclk;
        tx->vt_seq = vts->out_queue_counter;
        vts->clk_rw_mtx.unlock();

        // write tx in warp
        // gets/puts/dels node mappings
        // sets error if any warp operation returns error
        // sets upd->loc for each upd in tx
        // sets tx->shard_write bool_vector (shard_write[i] = true iff there is a tx component at shard i)
        hstub->do_tx(get_set, del_set, put_map, tx, ready, error, time_oracle);

        assert(!(ready && error)); // can't be ready after some error

        transaction::pending_tx *to_enq;
        if (!ready || error) {
            to_enq = tx->copy_fail_transaction();
        } else {
            to_enq = tx;
        }
        vts->enqueue_tx(to_enq);
    }

    message::message msg;
    if (error) {
        // fail tx
        delete tx;
        msg.prepare_message(message::CLIENT_TX_ABORT);
    } else {
        msg.prepare_message(message::CLIENT_TX_SUCCESS);
    }
    vts->comm.send_to_client(sender, msg.buf);
    vts->tx_queue_loop();
}

// if all replies have been received, ack to client
void
end_tx(uint64_t tx_id, uint64_t shard_id, coordinator::hyper_stub *hstub)
{
    vts->tx_prog_mutex.lock();
    auto find_iter = vts->outstanding_tx.find(tx_id);
    assert(find_iter != vts->outstanding_tx.end());

    transaction::pending_tx *tx = find_iter->second;
    uint64_t shard_idx = shard_id - ShardIdIncr;
    assert(tx->shard_write[shard_idx]);
    tx->shard_write[shard_idx] = false;

    if (weaver_util::none(tx->shard_write)) {
        // done tx
        hstub->clean_tx(tx_id);

        vts->outstanding_tx.erase(tx_id);
        vts->tx_prog_mutex.unlock();

        delete tx;
    } else {
        vts->tx_prog_mutex.unlock();
    }
}

// single dedicated thread which wakes up after given timeout, sends updates, and sleeps
void
nop_function()
{
    timespec sleep_time;
    int sleep_ret;
    int sleep_flags = 0;
    std::vector<uint64_t> del_done_reqs;
    transaction::pending_tx *tx = NULL;
    uint64_t num_shards;

    sleep_time.tv_sec  = VT_TIMEOUT_NANO / NANO;
    sleep_time.tv_nsec = VT_TIMEOUT_NANO % NANO;

    while (true) {
        sleep_ret = clock_nanosleep(CLOCK_REALTIME, sleep_flags, &sleep_time, NULL);
        assert((sleep_ret == 0 || sleep_ret == EINTR) && "error in clock_nanosleep");

        vts->periodic_update_mutex.lock();

        num_shards = get_num_shards();

        // send nops and state cleanup info to shards
        if (weaver_util::any(vts->to_nop)) {
            tx = new transaction::pending_tx(transaction::NOP);
            tx->nop = new transaction::nop_data();

            tx->id = vts->generate_req_id();
            vts->clk_rw_mtx.wrlock();
            vts->vclk.increment_clock();
            vts->out_queue_counter++;
            tx->timestamp = vts->vclk;
            tx->vt_seq = vts->out_queue_counter;
            tx->shard_write = vts->to_nop;
            vts->clk_rw_mtx.unlock();

            del_done_reqs.clear();
            vts->tx_prog_mutex.lock();
            tx->nop->max_done_id = vts->max_done_id;
            tx->nop->max_done_clk = *vts->max_done_clk;
            tx->nop->outstanding_progs = vts->pend_progs.size();
            tx->nop->shard_node_count = vts->shard_node_count;
            for (auto &x: vts->done_reqs) {
                // x.first = node prog type
                // x.second = unordered_map <req_id -> vector<bool>(NumShards)>
                for (auto &reply: x.second) {
                    // reply.first = req_id
                    // reply.second = vector<bool>(NumShards)
                    for (uint64_t shard_id = 0; shard_id < num_shards; shard_id++) {
                        if (vts->to_nop[shard_id] && (reply.second.size() > shard_id) && !reply.second[shard_id]) {
                            reply.second[shard_id] = true;
                            tx->nop->done_reqs[shard_id].emplace_back(std::make_pair(reply.first, x.first));
                        }
                    }
                    if (weaver_util::all(reply.second)) {
                        del_done_reqs.emplace_back(reply.first);
                    }
                }
                for (auto &del: del_done_reqs) {
                    x.second.erase(del);
                }
            }
            vts->tx_prog_mutex.unlock();

            weaver_util::reset_all(vts->to_nop);
        }

        vts->periodic_update_mutex.unlock();

        if (tx != NULL) {
            vts->enqueue_tx(tx);
            vts->tx_queue_loop();
            tx = NULL;
        }
    }
}

void
clk_update_function()
{
    timespec sleep_time;
    int sleep_ret;
    int sleep_flags = 0;
    message::message msg;
    vc::vclock vclk(vt_id, 0);
    uint64_t config_version;
    std::vector<server::state_t> vts_state(NumVts, server::NOT_AVAILABLE);

    sleep_time.tv_sec  = VT_CLK_TIMEOUT_NANO / NANO;
    sleep_time.tv_nsec = VT_CLK_TIMEOUT_NANO % NANO;

    vts->periodic_update_mutex.lock();
    config_version = vts->periodic_update_config.version();
    for (const server &srv: vts->periodic_update_config.get_servers()) {
        if (srv.type == server::VT && srv.state == server::AVAILABLE) {
            vts_state[srv.virtual_id] = server::AVAILABLE;
        }
    }
    vts->periodic_update_mutex.unlock();

    while (true) {
        sleep_ret = clock_nanosleep(CLOCK_REALTIME, sleep_flags, &sleep_time, NULL);
        if (sleep_ret != 0 && sleep_ret != EINTR) {
            assert(false);
        }
        vts->periodic_update_mutex.lock();

        if (config_version < vts->periodic_update_config.version()) {
            config_version = vts->periodic_update_config.version();

            for (server::state_t &s: vts_state) {
                s = server::NOT_AVAILABLE;
            }

            for (const server &srv: vts->periodic_update_config.get_servers()) {
                if (srv.type == server::VT && srv.state == server::AVAILABLE) {
                    vts_state[srv.virtual_id] = server::AVAILABLE;
                }
            }
        }

        // update vclock at other timestampers
        vts->clk_rw_mtx.rdlock();
        vclk.clock = vts->vclk.clock;
        vts->clk_rw_mtx.unlock();
        for (uint64_t i = 0; i < NumVts; i++) {
            if (i == vt_id || vts_state[i] != server::AVAILABLE) {
                continue;
            }
            msg.prepare_message(message::VT_CLOCK_UPDATE, vclk);
            vts->comm.send(i, msg.buf);
        }

        vts->periodic_update_mutex.unlock();
    }
}

// unpack client message for a node program, prepare shard msges, and send out
template <typename ParamsType, typename NodeStateType, typename CacheValueType>
void node_prog :: particular_node_program<ParamsType, NodeStateType, CacheValueType> :: 
    unpack_and_start_coord(std::unique_ptr<message::message> msg, uint64_t clientID, coordinator::hyper_stub *hstub)
{
    vts->restore_mtx.rdlock();
    if (vts->restore_status > 0) {
        std::unique_ptr<blocked_prog> to_save(new blocked_prog(clientID, std::move(msg)));
        vts->prog_queue.emplace_back(std::move(to_save));
        vts->restore_mtx.unlock();
        return;
    } else {
        vts->restore_mtx.unlock();
    }

    node_prog::prog_type pType;
    std::vector<std::pair<node_handle_t, ParamsType>> initial_args;

    msg->unpack_message(message::CLIENT_NODE_PROG_REQ, pType, initial_args);
    
    // map from locations to a list of start_node_params to send to that shard
    std::unordered_map<uint64_t, std::deque<std::pair<node_handle_t, ParamsType>>> initial_batches; 

    // lookup mappings
    std::unordered_map<node_handle_t, uint64_t> loc_map;
    std::unordered_set<node_handle_t> get_set;

    for (const auto &initial_arg : initial_args) {
        get_set.emplace(initial_arg.first);
    }

    if (!get_set.empty()) {
        loc_map = hstub->get_mappings(get_set);
        bool success = (loc_map.size() == get_set.size());

        if (!success) {
            // some node handles bad, return immediately
            WDEBUG << "bad node handles in node prog request" << std::endl;
            uint64_t zero = 0;
            msg->prepare_message(message::NODE_PROG_RETURN, pType, zero, ParamsType());
            vts->comm.send_to_client(clientID, msg->buf);
            return;
        }
    }

    for (auto &p: initial_args) {
        initial_batches[loc_map[p.first]].emplace_back(p);
    }

    vts->clk_rw_mtx.wrlock();
    vts->vclk.increment_clock();
    vc::vclock req_timestamp = vts->vclk;
    assert(req_timestamp.clock.size() == ClkSz);
    vts->clk_rw_mtx.unlock();

    vts->tx_prog_mutex.lock();
    uint64_t req_id = vts->generate_req_id();
    current_prog *cp = new current_prog(req_id, clientID, req_timestamp.clock);
    uint64_t cp_int = (uint64_t)cp;
    vts->pend_progs.emplace_back(cp);
    vts->tx_prog_mutex.unlock();

    message::message msg_to_send;
    for (auto &batch_pair: initial_batches) {
        msg_to_send.prepare_message(message::NODE_PROG, pType, vt_id, req_timestamp, req_id, cp_int, batch_pair.second);
        vts->comm.send(batch_pair.first, msg_to_send.buf);
    }

#ifdef weaver_benchmark_
    vts->test_mtx.lock();
    vts->outstanding_cnt++;
    if (vts->outstanding_cnt > vts->max_outstanding_cnt) {
        vts->max_outstanding_cnt = vts->outstanding_cnt;
    }
    vts->test_mtx.unlock();
#endif
}

template <typename ParamsType, typename NodeStateType, typename CacheValueType>
void node_prog :: particular_node_program<ParamsType, NodeStateType, CacheValueType> ::
    unpack_and_run_db(std::unique_ptr<message::message>, order::oracle*)
{ }

template <typename ParamsType, typename NodeStateType, typename CacheValueType>
void node_prog :: particular_node_program<ParamsType, NodeStateType, CacheValueType> ::
    unpack_context_reply_db(std::unique_ptr<message::message>, order::oracle*)
{ }

bool
compare_current_prog(const current_prog* const lhs, const current_prog* const rhs)
{
    return lhs->req_id < rhs->req_id;
}

// remove a completed node program from pending_prog data structure
// update 'max_done_id' and 'max_done_clk' accordingly
// caution: need to hold vts->tx_prog_mutex
void
node_prog_done(current_prog *cp)
{
    auto &pend_progs = vts->pend_progs;
    auto &done_progs = vts->done_progs;
    done_progs.emplace_back(cp);

    if (++vts->prog_done_cnt % 100 == 0) {
        vts->prog_done_cnt = 0;
    } else {
        return;
    }

    std::sort(pend_progs.begin(), pend_progs.end(), compare_current_prog);
    std::sort(done_progs.begin(), done_progs.end(), compare_current_prog);

    auto pend_iter = pend_progs.begin();
    auto done_iter = done_progs.begin();

    uint32_t i = 0;
    uint32_t max_idx = pend_progs.size() > done_progs.size() ? done_progs.size() : pend_progs.size();
    for (i = 0; i < max_idx && pend_progs[i]->req_id == done_progs[i]->req_id; i++, pend_iter++, done_iter++) {
        assert(vts->max_done_id < pend_progs[i]->req_id);
        vts->max_done_id = pend_progs[i]->req_id;
        vts->max_done_clk = std::move(pend_progs[i]->vclk);
        delete pend_progs[i];
    }

    if (i == pend_progs.size()) {
        pend_progs.clear();
    } else if (i > 0) {
        pend_progs.erase(pend_progs.begin(), pend_iter);
    }

    if (i == done_progs.size()) {
        done_progs.clear();
    } else if (i > 0) {
        done_progs.erase(done_progs.begin(), done_iter);
    }
}

void
server_loop(int thread_id)
{
    busybee_returncode ret;
    enum message::msg_type mtype;
    std::unique_ptr<message::message> msg;
    uint64_t tx_id, client_sender, shard_id;
    node_prog::prog_type pType;
    coordinator::hyper_stub *hstub = vts->hstub[thread_id];
    order::oracle *time_oracle = vts->time_oracles[thread_id];
    transaction::pending_tx *tx;

    while (true) {
        vts->comm.quiesce_thread(thread_id);
        msg.reset(new message::message());
        ret = vts->comm.recv(&client_sender, &msg->buf);
        if (ret != BUSYBEE_SUCCESS && ret != BUSYBEE_TIMEOUT) {
            continue;
        } else {
            // good to go, unpack msg
            mtype = msg->unpack_message_type();

            switch (mtype) {
                // client messages
                case message::CLIENT_TX_INIT: {
                    tx = new transaction::pending_tx(transaction::UPDATE);
                    msg->unpack_message(message::CLIENT_TX_INIT, tx->id, tx->writes);
                    tx->sender = client_sender;
                    prepare_tx(tx, hstub, time_oracle);
                    break;
                }

                case message::VT_CLOCK_UPDATE: {
                    vc::vclock rec_clk;
                    msg->unpack_message(message::VT_CLOCK_UPDATE, rec_clk);
                    vts->clk_rw_mtx.wrlock();
                    vts->clk_updates++;
                    vts->vclk.update_clock(rec_clk);
                    vts->clk_rw_mtx.unlock();
                    break;
                }

                //case message::VT_CLOCK_UPDATE_ACK:
                //    vts->periodic_update_mutex.lock();
                //    vts->clock_update_acks++;
                //    assert(vts->clock_update_acks < NumVts);
                //    vts->periodic_update_mutex.unlock();
                //    break;

                case message::VT_NOP_ACK: {
                    uint64_t shard_node_count, nop_qts, sid, sender;
                    msg->unpack_message(message::VT_NOP_ACK, sender, nop_qts, shard_node_count);
                    sid = sender - ShardIdIncr;
                    vts->periodic_update_mutex.lock();
                    if (nop_qts > vts->nop_ack_qts[sid]) {
                        vts->shard_node_count[sid] = shard_node_count;
                        vts->to_nop[sid] = true;
                        vts->nop_ack_qts[sid] = nop_qts;
                    }
                    vts->periodic_update_mutex.unlock();
                    break;
                }

                case message::CLIENT_NODE_COUNT: {
                    vts->periodic_update_mutex.lock();
                    msg->prepare_message(message::NODE_COUNT_REPLY, vts->shard_node_count);
                    vts->periodic_update_mutex.unlock();
                    vts->comm.send_to_client(client_sender, msg->buf);
                    break;
                }

                case message::TX_DONE:
                    msg->unpack_message(message::TX_DONE, tx_id, shard_id);
                    end_tx(tx_id, shard_id, hstub);
                    break;

                //case message::START_MIGR: {
                //    uint64_t hops = UINT64_MAX;
                //    msg->prepare_message(message::MIGRATION_TOKEN, hops, vt_id);
                //    vts->comm.send(ShardIdIncr, msg->buf); 
                //    break;
                //}

                case message::ONE_STREAM_MIGR: {
                    uint64_t hops = get_num_shards();
                    vts->migr_mutex.lock();
                    vts->migr_client = client_sender;
                    vts->migr_mutex.unlock();
                    msg->prepare_message(message::MIGRATION_TOKEN, hops, hops, vt_id);
                    vts->comm.send(ShardIdIncr, msg->buf);
                    break;
                }

                case message::MIGRATION_TOKEN: {
                    vts->migr_mutex.lock();
                    uint64_t client = vts->migr_client;
                    vts->migr_mutex.unlock();
                    msg->prepare_message(message::DONE_MIGR);
                    vts->comm.send_to_client(client, msg->buf);
                    WDEBUG << "Shard node counts are:";
                    for (uint64_t &x: vts->shard_node_count) {
                        std::cerr << " " << x;
                    }
                    std::cerr << std::endl;
                    break;
                }

                case message::CLIENT_NODE_PROG_REQ:
                    msg->unpack_partial_message(message::CLIENT_NODE_PROG_REQ, pType);
                    node_prog::programs.at(pType)->unpack_and_start_coord(std::move(msg), client_sender, hstub);
                    //msg.reset(new message::message());
                    //msg->prepare_message(message::NODE_PROG_RETURN, 0);
                    //vts->comm.send_to_client(client_sender, msg->buf);
                    break;

                // node program response from a shard
                case message::NODE_PROG_RETURN: {
                    uint64_t req_id, cp_int, client;
                    node_prog::prog_type type;
                    msg->unpack_partial_message(message::NODE_PROG_RETURN, type, req_id, cp_int); // don't unpack rest
                    current_prog *cp = (current_prog*)cp_int;
                    client = cp->client;
                    vts->comm.send_to_client(client, msg->buf);

                    vts->tx_prog_mutex.lock();
                    node_prog_done(cp);
                    vts->tx_prog_mutex.unlock();
#ifdef weaver_benchmark_
                    vts->test_mtx.lock();
                    vts->outstanding_cnt--;
                    vts->test_mtx.unlock();
#endif
                    break;
                }

                case message::RESTORE_DONE:
                    vts->restore_mtx.wrlock();
                    assert(vts->restore_status > 0);
                    vts->restore_status--;
                    coordinator::prog_queue_t blocked_progs = std::move(vts->prog_queue);
                    vts->restore_mtx.unlock();

                    vts->tx_queue_loop();
                    for (std::unique_ptr<blocked_prog> &bp: blocked_progs) {
                        bp->msg->unpack_partial_message(message::CLIENT_NODE_PROG_REQ, pType);
                        node_prog::programs.at(pType)->unpack_and_start_coord(std::move(bp->msg), bp->client, hstub);
                    }
                    break;

                default:
                    WDEBUG << "unexpected msg type " << message::to_string(mtype) << std::endl;
                    assert(false);
            }
        }
    }
}

bool
generate_token(uint64_t* token)
{
    po6::io::fd sysrand(open("/dev/urandom", O_RDONLY));

    if (sysrand.get() < 0)
    {
        return false;
    }

    if (sysrand.read(token, sizeof(*token)) != sizeof(*token))
    {
        return false;
    }

    return true;
}

void
server_manager_link_loop(po6::net::hostname sm_host, po6::net::location loc, bool backup)
{
    // Most of the following code has been 'borrowed' from
    // Robert Escriva's HyperDex.
    // see https://github.com/rescrv/HyperDex for the original code.

    vts->sm_stub.set_server_manager_address(sm_host.address.c_str(), sm_host.port);

    server::type_t type = backup? server::BACKUP_VT : server::VT;
    if (!vts->sm_stub.register_id(vts->serv_id, loc, type))
    {
        return;
    }

    bool cluster_jump = false;

    while (!vts->sm_stub.should_exit())
    {
        vts->exit_mutex.lock();
        if (vts->to_exit) {
            vts->sm_stub.request_shutdown();
            vts->to_exit = false;
        }
        vts->exit_mutex.unlock();

        if (!vts->sm_stub.maintain_link())
        {
            continue;
        }
        const configuration& old_config(vts->config);
        const configuration& new_config(vts->sm_stub.config());

        if (old_config.cluster() != 0 &&
            old_config.cluster() != new_config.cluster())
        {
            cluster_jump = true;
            break;
        }

        if (old_config.version() > new_config.version())
        {
            WDEBUG << "received new configuration version=" << new_config.version()
                   << " that's older than our current configuration version="
                   << old_config.version();
            continue;
        }
        // if old_config.version == new_config.version, still fetch

        vts->config_mutex.lock();
        vts->prev_config = vts->config;
        vts->config = new_config;
        if (!vts->first_config) {
            vts->first_config = true;
            vts->first_config_cond.signal();
        }
        vts->reconfigure();
        vts->config_mutex.unlock();

        // let the coordinator know we've moved to this config
        vts->sm_stub.config_ack(new_config.version());
    }

    if (cluster_jump)
    {
        WDEBUG << "\n================================================================================\n"
               << "Exiting because the server manager changed on us.\n"
               << "This is most likely an operations error."
               << "================================================================================";
    }
    else if (vts->sm_stub.should_exit() && !vts->sm_stub.config().exists(vts->serv_id))
    {
        WDEBUG << "\n================================================================================\n"
               << "Exiting because the server manager says it doesn't know about this node.\n"
               << "================================================================================";
    }
    else if (vts->sm_stub.should_exit())
    {
        WDEBUG << "\n================================================================================\n"
               << "Exiting because server manager stub says we should exit.\n"
               << "Most likely because we requested shutdown due to program interrupt.\n"
               << "================================================================================\n";
    }
    exit(0);
}

void
install_signal_handler(int signum, void (*handler)(int))
{
    struct sigaction sa;
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    int ret = sigaction(signum, &sa, NULL);
    assert(ret == 0);
}

// caution: assume holding vts->config_mutex for vts->pause_bb
void
init_worker_threads(std::vector<std::thread*> &threads)
{
    for (int i = 0; i < NUM_VT_THREADS; i++) {
        std::thread *t = new std::thread(server_loop, i);
        threads.emplace_back(t);
    }
    vts->pause_bb = true;
}

// caution: assume holding vts->config_mutex
void
init_vt()
{
    std::vector<server> servers = vts->config.get_servers();
    vt_id = UINT64_MAX;
    uint64_t weaver_id = UINT64_MAX;
    for (const server &srv: servers) {
        WDEBUG << "server " << srv.weaver_id << " has address " << srv.bind_to << std::endl;
        if (srv.id == vts->serv_id) {
            assert(srv.type == server::VT);
            vt_id = srv.virtual_id;
            weaver_id = srv.weaver_id;
        }
    }
    assert(vt_id != UINT64_MAX);

    // registered this server with server_manager, we now know the vt_id
    vts->init(vt_id, weaver_id);
}

int
main(int argc, const char *argv[])
{
    // signal handlers
    install_signal_handler(SIGINT, end_program);
    install_signal_handler(SIGHUP, end_program);
    install_signal_handler(SIGTERM, end_program);

    google::InitGoogleLogging(argv[0]);
    //google::InstallFailureSignalHandler();
    google::LogToStderr();
    //google::SetLogDestination(google::INFO, "weaver-timestamper-");

    // signals
    //sigset_t ss;
    //if (sigfillset(&ss) < 0) {
    //    WDEBUG << "sigfillset failed" << std::endl;
    //    return -1;
    //}
    //sigdelset(&ss, SIGPROF);
    //sigdelset(&ss, SIGINT);
    //sigdelset(&ss, SIGHUP);
    //sigdelset(&ss, SIGTERM);
    //sigdelset(&ss, SIGTSTP);
    //if (pthread_sigmask(SIG_SETMASK, &ss, NULL) < 0) {
    //    WDEBUG << "pthread sigmask failed" << std::endl;
    //    return -1;
    //}

    // command line params
    const char* listen_host = "127.0.0.1";
    long listen_port = 5200;
    const char *config_file = "./weaver.yaml";
    bool backup = false;
    // arg parsing borrowed from HyperDex
    e::argparser ap;
    ap.autohelp();
    ap.arg().name('l', "listen")
            .description("listen on a specific IP address (default: 127.0.0.1)")
            .metavar("IP").as_string(&listen_host);
    ap.arg().name('p', "listen-port")
            .description("listen on an alternative port (default: 5200)")
            .metavar("port").as_long(&listen_port);
    ap.arg().name('b', "backup-vt")
            .description("make this a backup timestamper")
            .set_true(&backup);
    ap.arg().long_name("config-file")
            .description("full path of weaver.yaml configuration file (default ./weaver.yaml)")
            .metavar("filename").as_string(&config_file);

    if (!ap.parse(argc, argv) || ap.args_sz() != 0) {
        WDEBUG << "args parsing failure" << std::endl;
        return -1;
    }

    // configuration file parse
    if (!init_config_constants(config_file)) {
        WDEBUG << "error in init_config_constants, exiting now." << std::endl;
        return -1;
    }

    po6::net::location my_loc(listen_host, listen_port);
    uint64_t sid;
    assert(generate_token(&sid));
    vts = new coordinator::timestamper(sid, my_loc, backup);

    // server manager link
    std::thread sm_thr(server_manager_link_loop,
        po6::net::hostname(ServerManagerIpaddr, ServerManagerPort),
        my_loc,
        backup);
    sm_thr.detach();

    vts->config_mutex.lock();

    // wait for first config to arrive from server manager
    while (!vts->first_config) {
        vts->first_config_cond.wait();
    }

    std::vector<std::thread*> worker_threads;

    if (backup) {
        while (!vts->active_backup) {
            vts->backup_cond.wait();
        }

        init_vt();

        vts->config_mutex.unlock();
        // release config_mutex while restoring vt which may take a while
        vts->restore_backup();

        vts->config_mutex.lock();
        init_worker_threads(worker_threads);
    } else {
        init_vt();

        init_worker_threads(worker_threads);
    }

    // initial wait for all vector timestampers to start
    while (vts->num_active_vts != NumVts) {
        vts->start_all_vts_cond.wait();
    }
    vts->config_mutex.unlock();

    std::cout << "Vector timestamper " << vt_id << std::endl;
    std::cout << "THIS IS AN ALPHA RELEASE WHICH DOES NOT SUPPORT FAULT TOLERANCE" << std::endl;

    if (NumVts > 1) {
        // periodic vector clock update to other timestampers
        std::thread clk_update_thr(clk_update_function);
        clk_update_thr.detach();
    }

    // periodic nops to shard
    nop_function();

    for (std::thread *t: worker_threads) {
        t->join();
    }

    return 0;
}
