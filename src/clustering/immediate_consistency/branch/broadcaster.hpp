// Copyright 2010-2013 RethinkDB, all rights reserved.
#ifndef CLUSTERING_IMMEDIATE_CONSISTENCY_BRANCH_BROADCASTER_HPP_
#define CLUSTERING_IMMEDIATE_CONSISTENCY_BRANCH_BROADCASTER_HPP_

#include <list>
#include <map>
#include <set>
#include <vector>

#include "utils.hpp"
#include <boost/shared_ptr.hpp>

#include "clustering/generic/registrar.hpp"
#include "clustering/immediate_consistency/branch/history.hpp"
#include "clustering/immediate_consistency/branch/metadata.hpp"
#include "concurrency/queue/unlimited_fifo.hpp"
#include "timestamps.hpp"

class listener_t;
template <class> class semilattice_readwrite_view_t;
class multistore_ptr_t;
class mailbox_manager_t;
class rdb_context_t;
class uuid_u;

class ack_checker_t : public home_thread_mixin_t {
public:
    virtual bool is_acceptable_ack_set(const std::set<server_id_t> &acks) const = 0;
    virtual write_durability_t get_write_durability() const = 0;

    ack_checker_t() { }
protected:
    virtual ~ack_checker_t() { }

private:
    DISABLE_COPYING(ack_checker_t);
};

/* Each shard has a `broadcaster_t` on its primary replica. Each server sends
queries via `cluster_namespace_interface_t` over the network to the `master_t`
on the primary replica server, which forwards the queries to the `broadcaster_t`.
From there, the `broadcaster_t` distributes them to one or more `listener_t`s.

When the `broadcaster_t` is first created, it generates a new unique branch ID.
The `broadcaster_t` is the authority on what sequence of operations is performed
on that branch. The order in which write and read operations pass through the
`broadcaster_t` is the order in which they are performed at the B-trees
themselves. */

class broadcaster_t : public home_thread_mixin_debug_only_t {
private:
    class incomplete_write_t;

public:
    class write_callback_t {
    public:
        write_callback_t();
        /* `on_success()` is called when a sufficiently large set of replicas have
        acknowledged the write, as judged by the `ack_checker_t` passed to
        `spawn_write()`. */
        virtual void on_success(const write_response_t &response) = 0;
        /* `on_failure()` is called when the `ack_checker_t` cannot be satisfied. If
        `might_have_run_anyway` is `true`, then the write might have been performed
        anyway; if `might_have_run_anyway` is `false`, then the write definitely wasn't
        performed. */
        virtual void on_failure(bool might_have_run_anyway) = 0;

    protected:
        virtual ~write_callback_t();

    private:
        friend class broadcaster_t;
        /* This is so that if the write callback is destroyed before `on_success()` or
        `on_failure()` is called, it will get deregistered. */
        incomplete_write_t *write;
    };

    broadcaster_t(
            mailbox_manager_t *mm,
            rdb_context_t *rdb_context,
            branch_history_manager_t *bhm,
            store_view_t *initial_svs,
            perfmon_collection_t *parent_perfmon_collection,
            order_source_t *order_source,
            signal_t *interruptor) THROWS_ONLY(interrupted_exc_t);

    void read(
        const read_t &r,
        read_response_t *response,
        fifo_enforcer_sink_t::exit_read_t *lock,
        order_token_t tok,
        signal_t *interruptor)
        THROWS_ONLY(cannot_perform_query_exc_t, interrupted_exc_t);

    /* Unlike `read()`, `spawn_write()` returns as soon as the write has begun
    and replies asynchronously via a callback. It may block, so it takes an
    `interruptor`, but it shouldn't block for a long time. If the
    `write_callback_t` is destroyed while the write is still in progress, its
    destructor will automatically deregister it so that no segfaults will
    happen. */
    void spawn_write(const write_t &w,
                     fifo_enforcer_sink_t::exit_write_t *lock,
                     order_token_t tok,
                     write_callback_t *cb,
                     signal_t *interruptor,
                     const ack_checker_t *ack_checker) THROWS_ONLY(interrupted_exc_t);

    branch_id_t get_branch_id() const;

    broadcaster_business_card_t get_business_card();

    MUST_USE store_view_t *release_bootstrap_svs_for_listener();

    void register_local_listener(const uuid_u &listener_id,
                                 listener_t *listener,
                                 auto_drainer_t::lock_t listener_keepalive);

private:
    class incomplete_write_ref_t;

    class dispatchee_t;

    /* Reads need to pick a single readable mirror to perform the operation.
    Writes need to choose a readable mirror to get the reply from. Both use
    `pick_a_readable_dispatchee()` to do the picking. You must hold
    `dispatchee_mutex` and pass in `proof` of the mutex acquisition. (A
    dispatchee is "readable" if a `replier_t` exists for it on the remote
    server.) */
    void pick_a_readable_dispatchee(
        dispatchee_t **dispatchee_out, mutex_assertion_t::acq_t *proof,
        auto_drainer_t::lock_t *lock_out) THROWS_ONLY(cannot_perform_query_exc_t);
    void get_all_readable_dispatchees(
        std::vector<dispatchee_t *> *dispatchees_out, mutex_assertion_t::acq_t *proof,
        std::vector<auto_drainer_t::lock_t> *locks_out)
        THROWS_ONLY(cannot_perform_query_exc_t);

    void background_write(
        dispatchee_t *mirror, auto_drainer_t::lock_t mirror_lock,
        incomplete_write_ref_t write_ref, order_token_t order_token,
        fifo_enforcer_write_token_t token) THROWS_NOTHING;
    void background_writeread(
        dispatchee_t *mirror, auto_drainer_t::lock_t mirror_lock,
        incomplete_write_ref_t write_ref, order_token_t order_token,
        fifo_enforcer_write_token_t token, write_durability_t durability) THROWS_NOTHING;
    void end_write(boost::shared_ptr<incomplete_write_t> write) THROWS_NOTHING;

    void single_read(
        const read_t &r,
        read_response_t *response,
        fifo_enforcer_sink_t::exit_read_t *lock, order_token_t tok,
        signal_t *interruptor)
        THROWS_ONLY(cannot_perform_query_exc_t, interrupted_exc_t);

    void all_read(
        const read_t &r,
        read_response_t *response,
        fifo_enforcer_sink_t::exit_read_t *lock, order_token_t tok,
        signal_t *interruptor)
        THROWS_ONLY(cannot_perform_query_exc_t, interrupted_exc_t);

    void listener_read(
        broadcaster_t::dispatchee_t *mirror,
        const read_t &r, read_response_t *response, state_timestamp_t ts,
        order_token_t order_token, fifo_enforcer_read_token_t token,
        signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t);

    void listener_write(
        broadcaster_t::dispatchee_t *mirror,
        const write_t &w, state_timestamp_t timestamp,
        order_token_t order_token, fifo_enforcer_write_token_t token,
        signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t);

    /* This recomputes `readable_dispatchees_as_set()` from `readable_dispatchees()` */
    void refresh_readable_dispatchees_as_set();

    /* This function sanity-checks `incomplete_writes`, `current_timestamp`,
    and `newest_complete_timestamp`. It mostly exists as a form of executable
    documentation. */
    void sanity_check();

    perfmon_collection_t broadcaster_collection;
    perfmon_membership_t broadcaster_membership;

    rdb_context_t *const rdb_context;

    mailbox_manager_t *const mailbox_manager;

    const branch_id_t branch_id;

    /* Until our initial listener has been constructed, this holds the
    store_view that was passed to our constructor. After that, it's
    `NULL`. */
    store_view_t *bootstrap_svs;

    branch_history_manager_t *const branch_history_manager;

    /* If a write has begun, but some mirror might not have completed it yet,
    then it goes in `incomplete_writes`. The idea is that a new mirror that
    connects will use the union of a backfill and `incomplete_writes` as its
    data, and that will guarantee it gets at least one copy of every write.
    See the member function `sanity_check()` for a description of the
    relationship between `incomplete_writes`, `current_timestamp`, and
    `newest_complete_timestamp`. */

    /* `mutex` is held by new writes and reads being created, by writes
    finishing, and by dispatchees joining, leaving, or upgrading. It protects
    `incomplete_writes`, `current_timestamp`, `newest_complete_timestamp`,
    `order_checkpoint`, `dispatchees`, and `readable_dispatchees`. */
    mutex_assertion_t mutex;

    std::list<boost::shared_ptr<incomplete_write_t> > incomplete_writes;
    state_timestamp_t current_timestamp, newest_complete_timestamp;
    order_checkpoint_t order_checkpoint;

    std::map<dispatchee_t *, auto_drainer_t::lock_t> dispatchees;
    intrusive_list_t<dispatchee_t> readable_dispatchees;

    /* This is just a set that contains the peer ID of each dispatchee in
    `readable_dispatchees`. We keep it as a `std::set` because we need to pass it to
    `ack_checker->is_acceptable_ack_set()` for every single write operation, and this
    avoids the overhead of recreating the `std::set` each time. */
    std::set<server_id_t> readable_dispatchees_as_set;

    registrar_t<listener_business_card_t, broadcaster_t *, dispatchee_t>
        registrar;

    DISABLE_COPYING(broadcaster_t);
};

#endif /* CLUSTERING_IMMEDIATE_CONSISTENCY_BRANCH_BROADCASTER_HPP_ */