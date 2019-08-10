/************************************************************************
Modifications Copyright 2017-2019 eBay Inc.
Author/Developer(s): Jung-Sang Ahn

Original Copyright:
See URL: https://github.com/datatechnology/cornerstone

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
**************************************************************************/

#include "raft_server.hxx"

#include "cluster_config.hxx"
#include "error_code.hxx"
#include "event_awaiter.h"
#include "peer.hxx"
#include "snapshot.hxx"
#include "state_machine.hxx"
#include "state_mgr.hxx"
#include "tracer.hxx"

#include <algorithm>
#include <cassert>
#include <sstream>

namespace nuraft {

void raft_server::request_append_entries() {
    // Special case:
    //   1) one-node cluster, OR
    //   2) quorum size == 1 (including leader).
    //
    // In those cases, we may not enter `handle_append_entries_resp`,
    // which calls `commit()` function.
    // We should call it here.
    if ( peers_.size() == 0 ||
         get_quorum_for_commit() == 0 ) {
        commit(log_store_->next_slot() - 1);
        return;
    }

    for (peer_itor it = peers_.begin(); it != peers_.end(); ++it) {
        request_append_entries(it->second);
    }
}

bool raft_server::request_append_entries(ptr<peer> p) {
    cb_func::Param cb_param(id_, leader_, p->get_id());
    CbReturnCode rc = ctx_->cb_func_.call(cb_func::RequestAppendEntries, &cb_param);
    if (rc == CbReturnCode::ReturnNull) {
        p_wn("by callback, abort request_append_entries");
        return true;
    }

    ptr<raft_params> params = ctx_->get_params();

    bool need_to_reconnect = p->need_to_reconnect();
    int32 last_active_time_ms = p->get_active_timer_us() / 1000;
    if ( last_active_time_ms >
             params->heart_beat_interval_ * peer::RECONNECT_LIMIT ) {
        p_wn( "connection to peer %d is not active long time: %zu ms, "
              "force re-connect",
              p->get_id(),
              last_active_time_ms );
        need_to_reconnect = true;
        p->reset_active_timer();
    }
    if (need_to_reconnect) {
        reconnect_client(*p);
        p->clear_reconnection();
    }

    if (p->make_busy()) {
        p_tr("send request to %d\n", (int)p->get_id());
        ptr<req_msg> msg = create_append_entries_req(*p);
        if (!msg) {
            p->set_free();
            return true;
        }

        if (!p->is_manual_free()) {
            // Actual recovery.
            if (p->get_long_puase_warnings() >= peer::WARNINGS_LIMIT) {
                int32 last_ts_ms = p->get_ls_timer_us() / 1000;
                p->inc_recovery_cnt();
                p_wn( "recovered from long pause to peer %d, %d warnings, "
                      "%d ms, %d times",
                      p->get_id(),
                      p->get_long_puase_warnings(),
                      last_ts_ms,
                      p->get_recovery_cnt() );

                if (p->get_recovery_cnt() >= 10) {
                    // Re-connect client, just in case.
                    //reconnect_client(*p);
                    p->reset_recovery_cnt();
                }
            }
            p->reset_long_pause_warnings();

        } else {
            // It means that this is not an actual recovery,
            // but just temporarily freed busy flag.
            p->reset_manual_free();
        }

        p->send_req(p, msg, resp_handler_);
        p->reset_ls_timer();
        p_tr("sent\n");
        return true;
    }

    p_db("Server %d is busy, skip the request", p->get_id());

    int32 last_ts_ms = p->get_ls_timer_us() / 1000;
    if ( last_ts_ms > params->heart_beat_interval_ ) {
        // Waiting time becomes longer than HB interval, warning.
        p->inc_long_pause_warnings();
        if (p->get_long_puase_warnings() < peer::WARNINGS_LIMIT) {
            p_wn("skipped sending msg to %d too long time, "
                 "last msg sent %d ms ago",
                 p->get_id(), last_ts_ms);

        } else if (p->get_long_puase_warnings() == peer::WARNINGS_LIMIT) {
            p_wn("long pause warning to %d is too verbose, "
                 "will suppress it from now", p->get_id());
        }

        // For resiliency, free busy flag once to send heartbeat to the peer.
        if ( last_ts_ms > params->heart_beat_interval_ *
                          peer::BUSY_FLAG_LIMIT ) {
            p_wn("probably something went wrong. "
                 "temporarily free busy flag for peer %d", p->get_id());
            p->set_free();
            p->set_manual_free();
            p->reset_ls_timer();
        }
    }
    return false;
}

ptr<req_msg> raft_server::create_append_entries_req(peer& p) {
    ulong cur_nxt_idx(0L);
    ulong commit_idx(0L);
    ulong last_log_idx(0L);
    ulong term(0L);
    ulong starting_idx(1L);

    {
        recur_lock(lock_);
        starting_idx = log_store_->start_index();
        cur_nxt_idx = log_store_->next_slot();
        commit_idx = quick_commit_index_;
        term = state_->get_term();
    }

    {
        std::lock_guard<std::mutex> guard(p.get_lock());
        if (p.get_next_log_idx() == 0L) {
            p.set_next_log_idx(cur_nxt_idx);
        }

        last_log_idx = p.get_next_log_idx() - 1;
    }

    if (last_log_idx >= cur_nxt_idx) {
        // LCOV_EXCL_START
        p_er( "Peer's lastLogIndex is too large %llu v.s. %llu, ",
              last_log_idx, cur_nxt_idx );
        ctx_->state_mgr_->system_exit(raft_err::N8_peer_last_log_idx_too_large);
        ::exit(-1);
        return ptr<req_msg>();
        // LCOV_EXCL_STOP
    }

    // cur_nxt_idx: last log index of myself (leader).
    // starting_idx: start log index of myself (leader).
    // last_log_idx: last log index of replica (follower).
    // end_idx: if (cur_nxt_idx - last_log_idx) > threshold (100), limit it.

    p_tr("last_log_idx: %d, starting_idx: %d, cur_nxt_idx: %d\n",
         last_log_idx, starting_idx, cur_nxt_idx);

    // To avoid inconsistency due to smart pointer, should have local varaible
    // to increase its ref count.
    ptr<snapshot> snp_local = get_last_snapshot();

    // Modified by Jung-Sang Ahn (Oct 11 2017):
    // As `reserved_log` has been newly added, checking with `starting_idx` only
    // is inaccurate.
    if ( snp_local &&
         last_log_idx < starting_idx &&
         last_log_idx < snp_local->get_last_log_idx() ) {
        p_db( "send snapshot peer %d, peer log idx: %zu, my starting idx: %zu, "
              "my log idx: %zu, last_snapshot_log_idx: %zu\n",
              p.get_id(),
              last_log_idx, starting_idx, cur_nxt_idx,
              snp_local->get_last_log_idx() );
        return create_sync_snapshot_req(p, last_log_idx, term, commit_idx);
    }

    ulong last_log_term = term_for_log(last_log_idx);
    ulong end_idx = std::min( cur_nxt_idx,
                              last_log_idx + 1 +
                                  ctx_->get_params()->max_append_size_ );
    // NOTE: If this is a retry, probably the follower is down.
    //       Send just one log until it comes back
    //       (i.e., max_append_size_ = 1).
    //       Only when end_idx - start_idx > 1, and 5th try.
    ulong peer_last_sent_idx = p.get_last_sent_idx();
    if ( last_log_idx + 1 == peer_last_sent_idx &&
         last_log_idx + 2 < end_idx ) {
        int32 cur_cnt = p.inc_cnt_not_applied();
        p_db("last sent log (%zu) to peer %d is not applied, cnt %d",
             peer_last_sent_idx, p.get_id(), cur_cnt);
        if (cur_cnt >= 5) {
            ulong prev_end_idx = end_idx;
            end_idx = std::min( cur_nxt_idx, last_log_idx + 1 + 1 );
            p_db("reduce end_idx %zu -> %zu", prev_end_idx, end_idx);
        }
    } else {
        p.reset_cnt_not_applied();
    }

    ptr<std::vector<ptr<log_entry>>>
        log_entries( (last_log_idx + 1) >= cur_nxt_idx
                     ? ptr<std::vector<ptr<log_entry>>>()
                     : log_store_->log_entries(last_log_idx + 1, end_idx) );
    p_db( "append_entries for %d with LastLogIndex=%llu, "
          "LastLogTerm=%llu, EntriesLength=%d, CommitIndex=%llu, "
          "Term=%llu, peer_last_sent_idx %zu",
          p.get_id(), last_log_idx, last_log_term,
          ( log_entries ? log_entries->size() : 0 ), commit_idx, term,
          peer_last_sent_idx );
    if (last_log_idx+1 == end_idx) {
        p_tr( "EMPTY PAYLOAD" );
    } else if (last_log_idx+1 + 1 == end_idx) {
        p_db( "idx: %zu", last_log_idx+1 );
    } else {
        p_db( "idx range: %zu-%zu", last_log_idx+1, end_idx-1 );
    }

    ptr<req_msg> req
        ( cs_new<req_msg>
          ( term, msg_type::append_entries_request, id_, p.get_id(),
            last_log_term, last_log_idx, commit_idx ) );
    std::vector<ptr<log_entry>>& v = req->log_entries();
    if (log_entries) {
        v.insert(v.end(), log_entries->begin(), log_entries->end());
    }
    p.set_last_sent_idx(last_log_idx + 1);

    return req;
}

ptr<resp_msg> raft_server::handle_append_entries(req_msg& req)
{
    bool supp_exp_warning = false;
    if (catching_up_) {
        p_in("catch-up process is done, "
             "will suppress following expected warnings this time");
        catching_up_ = false;
        supp_exp_warning = true;
    }

    // To avoid election timer wakes up while we are in the middle
    // of this function, this structure sets the flag and automatically
    // clear it when we return from this function.
    struct ServingReq {
        ServingReq(std::atomic<bool>* _val) : val(_val) { val->store(true); }
        ~ServingReq() { val->store(false); }
        std::atomic<bool>* val;
    } _s_req(&serving_req_);
    timer_helper tt;

    p_tr("from peer %d, req type: %d, req term: %ld, "
         "req l idx: %ld (%zu), req c idx: %ld, "
         "my term: %ld, my role: %d\n",
         req.get_src(), (int)req.get_type(), req.get_term(),
         req.get_last_log_idx(), req.log_entries().size(), req.get_commit_idx(),
         state_->get_term(), (int)role_);

    if (req.get_term() == state_->get_term()) {
        if (role_ == srv_role::candidate) {
            become_follower();
        } else if (role_ == srv_role::leader) {
            p_wn( "Receive AppendEntriesRequest from another leader (%d) "
                  "with same term, there must be a bug. Ignore it instead of exit.",
                  req.get_src() );
            return nullptr;
        } else {
            update_target_priority();
            // Modified by JungSang Ahn, Mar 28 2018:
            //   As we have `serving_req_` flag, restarting election timer
            //   should be move to the end of this function.
            // restart_election_timer();
        }
    }

    // After a snapshot the req.get_last_log_idx() may less than
    // log_store_->next_slot() but equals to log_store_->next_slot() -1
    //
    // In this case, log is Okay if
    //   req.get_last_log_idx() == lastSnapshot.get_last_log_idx() &&
    //   req.get_last_log_term() == lastSnapshot.get_last_log_term()
    //
    // In not accepted case, we will return log_store_->next_slot() for
    // the leader to quick jump to the index that might aligned.
    ptr<resp_msg> resp = cs_new<resp_msg>( state_->get_term(),
                                           msg_type::append_entries_response,
                                           id_,
                                           req.get_src(),
                                           log_store_->next_slot() );

    ptr<snapshot> local_snp = get_last_snapshot();
    ulong log_term = 0;
    if (req.get_last_log_idx() < log_store_->next_slot()) {
        log_term = term_for_log( req.get_last_log_idx() );
    }
    bool log_okay =
            req.get_last_log_idx() == 0 ||
            ( log_term &&
              req.get_last_log_term() == log_term ) ||
            ( local_snp &&
              local_snp->get_last_log_idx() == req.get_last_log_idx() &&
              local_snp->get_last_log_term() == req.get_last_log_term() );

    p_lv( (log_okay ? L_TRACE : (supp_exp_warning ? L_INFO : L_WARN) ),
          "[LOG %s] req log idx: %zu, req log term: %zu, my last log idx: %zu, "
          "my log (%zu) term: %zu",
          (log_okay ? "OK" : "XX"),
          req.get_last_log_idx(),
          req.get_last_log_term(),
          log_store_->next_slot() - 1,
          req.get_last_log_idx(),
          log_term );

    if ( req.get_term() < state_->get_term() ||
         log_okay == false ) {
        p_lv( (supp_exp_warning ? L_INFO : L_WARN),
              "deny, req term %zu, my term %zu, req log idx %zu, my log idx %zu",
              req.get_term(), state_->get_term(),
              req.get_last_log_idx(), log_store_->next_slot() - 1 );
        if (local_snp) {
            p_wn("snp idx %zu term %zu",
                 local_snp->get_last_log_idx(),
                 local_snp->get_last_log_term());
        }
        return resp;
    }

    // --- Now this node is a follower, and given log is okay. ---

    // set initialized flag
    if (!initialized_) initialized_ = true;

    // Callback if necessary.
    cb_func::Param param(id_, leader_, -1, &req);
    ctx_->cb_func_.call(cb_func::GotAppendEntryReqFromLeader, &param);

    if (req.log_entries().size() > 0) {
        // Write logs to store, start from overlapped logs

        // Actual log number.
        ulong log_idx = req.get_last_log_idx() + 1;
        // Local counter for iterating req.log_entries().
        size_t cnt = 0;

        p_db("[INIT] log_idx: %ld, count: %ld, log_store_->next_slot(): %ld, "
             "req.log_entries().size(): %ld\n",
             log_idx, cnt, log_store_->next_slot(), req.log_entries().size());

        // Skipping already existing (with the same term) logs.
        while ( log_idx < log_store_->next_slot() &&
                cnt < req.log_entries().size() )
        {
            if ( log_store_->term_at(log_idx) ==
                     req.log_entries().at(cnt)->get_term() ) {
                log_idx++;
                cnt++;
            } else {
                break;
            }
        }
        p_db("[after SKIP] log_idx: %ld, count: %ld\n", log_idx, cnt);

        // Dealing with overwrites (logs with different term).
        while ( log_idx < log_store_->next_slot() &&
                cnt < req.log_entries().size() )
        {
            ptr<log_entry> old_entry = log_store_->entry_at(log_idx);
            if (old_entry->get_val_type() == log_val_type::app_log) {
                ptr<buffer> buf = old_entry->get_buf_ptr();
                buf->pos(0);
                state_machine_->rollback_ext
                    ( state_machine::ext_op_params( log_idx, buf ) );

            } else if (old_entry->get_val_type() == log_val_type::conf) {
                p_in( "revert from a prev config change to config at %llu",
                      get_config()->get_log_idx() );
                config_changing_ = false;
            }

            ptr<log_entry> entry = req.log_entries().at(cnt);
            p_db("write at %d\n", (int)log_idx);
            store_log_entry(entry, log_idx);

            if (entry->get_val_type() == log_val_type::app_log) {
                ptr<buffer> buf = entry->get_buf_ptr();
                buf->pos(0);
                state_machine_->pre_commit_ext
                    ( state_machine::ext_op_params( log_idx, buf ) );

            } else if(entry->get_val_type() == log_val_type::conf) {
                p_in("receive a config change from leader at %llu", log_idx);
                config_changing_ = true;
            }

            // if rollback point is smaller than commit index,
            // should rollback commit index as well.
            if (log_idx <= sm_commit_index_) {
                p_wn("rollback commit index from %zu to %zu",
                     sm_commit_index_.load(), log_idx - 1);
                sm_commit_index_ = log_idx - 1;
                quick_commit_index_ = log_idx - 1;
            }

            log_idx += 1;
            cnt += 1;

            if (stopping_) return resp;
        }
        p_db("[after OVWR] log_idx: %ld, count: %ld\n", log_idx, cnt);

        // Append new log entries
        while (cnt < req.log_entries().size()) {
            p_tr("append at %d\n", (int)log_store_->next_slot());
            ptr<log_entry> entry = req.log_entries().at( cnt++ );
            ulong idx_for_entry = store_log_entry(entry);
            if (entry->get_val_type() == log_val_type::conf) {
                p_in( "receive a config change from leader at %llu",
                      idx_for_entry );
                config_changing_ = true;

            } else if(entry->get_val_type() == log_val_type::app_log) {
                ptr<buffer> buf = entry->get_buf_ptr();
                buf->pos(0);
                state_machine_->pre_commit_ext
                    ( state_machine::ext_op_params( idx_for_entry, buf ) );
            }

            if (stopping_) return resp;
        }

        // End of batch.
        log_store_->end_of_append_batch( req.get_last_log_idx() + 1,
                                         req.log_entries().size() );
    }

    leader_ = req.get_src();
    leader_commit_index_.store(req.get_commit_idx());

    // WARNING:
    //   If `commit_idx > next_slot()`, it may cause problem
    //   on next `append_entries()` call, due to racing
    //   between BG commit thread and appending logs.
    //   Hence, we always should take smaller one.
    commit( std::min( req.get_commit_idx(),
                      log_store_->next_slot() - 1 ) );

    resp->accept(req.get_last_log_idx() + req.log_entries().size() + 1);

    int32 time_ms = tt.get_us() / 1000;
    if (time_ms >= ctx_->get_params()->heart_beat_interval_) {
        // Append entries took longer than HB interval. Warning.
        p_wn("appending entries from peer %d took long time (%d ms)\n"
             "req type: %d, req term: %ld, "
             "req l idx: %ld (%zu), req c idx: %ld, "
             "my term: %ld, my role: %d\n",
             req.get_src(), time_ms, (int)req.get_type(), req.get_term(),
             req.get_last_log_idx(), req.log_entries().size(), req.get_commit_idx(),
             state_->get_term(), (int)role_);
    }

    // Modified by Jung-Sang Ahn, Mar 28 2018.
    // Restart election timer here, as this function may take long time.
    if ( req.get_term() == state_->get_term() &&
         role_ == srv_role::follower ) {
        restart_election_timer();
    }

    return resp;
}

void raft_server::handle_append_entries_resp(resp_msg& resp) {
    peer_itor it = peers_.find(resp.get_src());
    if (it == peers_.end()) {
        p_in("the response is from an unknown peer %d", resp.get_src());
        return;
    }

    // if there are pending logs to be synced or commit index need to be advanced,
    // continue to send appendEntries to this peer
    bool need_to_catchup = true;
    ptr<peer> p = it->second;
    p_tr("handle append entries resp (from %d), resp.get_next_idx(): %d\n",
         (int)p->get_id(), (int)resp.get_next_idx());
    if (resp.get_accepted()) {
        uint64_t prev_matched_idx = 0;
        uint64_t new_matched_idx = 0;
        {
            std::lock_guard<std::mutex>(p->get_lock());
            p->set_next_log_idx(resp.get_next_idx());
            prev_matched_idx = p->get_matched_idx();
            new_matched_idx = resp.get_next_idx() - 1;
            p_tr("peer %d, prev idx: %ld, nex idx: %ld",
                 p->get_id(), prev_matched_idx, new_matched_idx);
            p->set_matched_idx(new_matched_idx);
        }
        cb_func::Param param(id_, leader_, p->get_id());
        param.ctx = &new_matched_idx;
        CbReturnCode rc = ctx_->cb_func_.call
                          ( cb_func::GotAppendEntryRespFromPeer, &param );
        (void)rc;

        // Try to commit with this response.
        std::vector<ulong> matched_indexes;
        matched_indexes.reserve(16);

        // Leader itself.
        matched_indexes.push_back( log_store_->next_slot() - 1 );
        for (auto& entry: peers_) {
            ptr<peer>& p = entry.second;

            // Skip learner.
            if (p->is_learner()) continue;

            matched_indexes.push_back( p->get_matched_idx() );
        }
        assert((int32)matched_indexes.size() == get_num_voting_members());

        // NOTE: Descending order.
        //       e.g.) 100 100 99 95 92
        //             => commit on 99 if `quorum_idx == 2`.
        std::sort( matched_indexes.begin(),
                   matched_indexes.end(),
                   std::greater<ulong>() );

        size_t quorum_idx = get_quorum_for_commit();
        if (l_->get_level() >= 6) {
            std::string tmp_str;
            for (ulong m_idx: matched_indexes) {
                tmp_str += std::to_string(m_idx) + " ";
            }
            p_tr("quorum idx %zu, %s", quorum_idx, tmp_str.c_str());
        }

        commit( matched_indexes[ quorum_idx ] );
        need_to_catchup = p->clear_pending_commit() ||
                          resp.get_next_idx() < log_store_->next_slot();

    } else {
        ulong prev_next_log = p->get_next_log_idx();
        std::lock_guard<std::mutex> guard(p->get_lock());
        if (resp.get_next_idx() > 0 && p->get_next_log_idx() > resp.get_next_idx()) {
            // fast move for the peer to catch up
            p->set_next_log_idx(resp.get_next_idx());
        } else {
            // if not, move one log backward.
            p->set_next_log_idx(p->get_next_log_idx() - 1);
        }
        bool suppress = p->need_to_suppress_error();
        p_lv( (suppress ? L_INFO : L_WARN),
              "declined append: peer %d, prev next log idx %zu, "
              "resp next %zu, new next log idx %zu",
              p->get_id(), prev_next_log,
              resp.get_next_idx(), p->get_next_log_idx() );
    }

    // This may not be a leader anymore,
    // such as the response was sent out long time ago
    // and the role was updated by UpdateTerm call
    // Try to match up the logs for this peer
    if (role_ == srv_role::leader && need_to_catchup) {
        p_db("reqeust append entries need to catchup, p %d\n",
             (int)p->get_id());
        request_append_entries(p);
    }
}

} // namespace nuraft;
