#ifndef CLUSTERING_REACTOR_DIRECTORY_ECHO_TCC_
#define CLUSTERING_REACTOR_DIRECTORY_ECHO_TCC_

template<class internal_t>
directory_echo_writer_t<internal_t>::our_value_change_t::our_value_change_t(directory_echo_writer_t *p) :
        parent(p), lock_acq(&parent->value_lock), buffer(parent->value_watchable.get_watchable()->get().internal) { }

template <class internal_t>
directory_echo_version_t directory_echo_writer_t<internal_t>::our_value_change_t::commit() {
    parent->version++;
    parent->value_watchable.set_value(
        directory_echo_wrapper_t<internal_t>(
            buffer,
            parent->version,
            parent->ack_mailbox.get_address()
            )
        );
    return parent->version;
}

template<class internal_t>
directory_echo_writer_t<internal_t>::ack_waiter_t::ack_waiter_t(directory_echo_writer_t *parent, peer_id_t peer, directory_echo_version_t version) {
    mutex_assertion_t::acq_t acq(&parent->ack_lock);
    std::map<peer_id_t, directory_echo_version_t>::iterator it = parent->last_acked.find(peer);
    if (it != parent->last_acked.end() && (*it).second >= version) {
        pulse();
        return;
    }
    typename std::map<peer_id_t, std::multimap<directory_echo_version_t, ack_waiter_t *> >::iterator it2 =
        parent->waiters.find(peer);
    if (it2 == parent->waiters.end()) {
        it2 = parent->waiters.insert(std::make_pair(peer, std::multimap<directory_echo_version_t, ack_waiter_t *>())).first;
    }
    map_entry.reset(
        new multimap_insertion_sentry_t<directory_echo_version_t, ack_waiter_t *>(
            &(*it2).second, version, this
            )
        );
}

template<class internal_t>
directory_echo_writer_t<internal_t>::directory_echo_writer_t(
        mailbox_manager_t *mm,
        const internal_t &initial) :
    ack_mailbox(mm, boost::bind(&directory_echo_writer_t<internal_t>::on_ack, this, _1, _2, auto_drainer_t::lock_t(&drainer))),
    value_watchable(directory_echo_wrapper_t<internal_t>(initial, 0, ack_mailbox.get_address())),
    version(0)
    { }

template<class internal_t>
void directory_echo_writer_t<internal_t>::on_ack(peer_id_t peer, directory_echo_version_t version, auto_drainer_t::lock_t lock) {
    lock.assert_is_holding(&drainer);
    mutex_assertion_t::acq_t acq(&ack_lock);
    std::map<peer_id_t, directory_echo_version_t>::iterator it = last_acked.find(peer);
    if (it == last_acked.end() || (*it).second < version) {
        last_acked[peer] = version;
    }
    typename std::map<peer_id_t, std::multimap<directory_echo_version_t, ack_waiter_t *> >::iterator it2 =
        waiters.find(peer);
    if (it2 != waiters.end()) {
        for (typename std::multimap<directory_echo_version_t, ack_waiter_t *>::iterator it3 = (*it2).second.begin();
                it3 != (*it2).second.upper_bound(version); it3++) {
            if (!(*it3).second->is_pulsed()) {
                (*it3).second->pulse();
            }
        }
    }
}

template<class internal_t>
directory_echo_mirror_t<internal_t>::directory_echo_mirror_t(
        mailbox_manager_t *mm,
        const clone_ptr_t<watchable_t<std::map<peer_id_t, directory_echo_wrapper_t<internal_t> > > > &p) :
    mailbox_manager(mm), peers(p),
    sub(boost::bind(&directory_echo_mirror_t::on_change, this)) {

    typename watchable_t<std::map<peer_id_t, directory_echo_wrapper_t<internal_t> > >::freeze_t freeze(peers);
    sub.reset(peers, &freeze);
    on_change();
}

template<class internal_t>
void directory_echo_mirror_t<internal_t>::on_change() {
    std::map<peer_id_t, directory_echo_wrapper_t<internal_t> > snapshot = peers->get();
    for (typename std::map<peer_id_t, directory_echo_wrapper_t<internal_t> >::iterator it = snapshot.begin(); it != snapshot.end(); it++) {
        int version = it->second.version;
        std::map<peer_id_t, directory_echo_version_t>::iterator jt = last_seen.find(it->first);
        if (jt == last_seen.end() || jt->second < version) {
            last_seen[it->first] = version;
            coro_t::spawn_sometime(boost::bind(
                &directory_echo_mirror_t<internal_t>::ack_version, this,
                it->second.ack_mailbox, version,
                auto_drainer_t::lock_t(&drainer)
                ));
        }
    }
}

template<class internal_t>
void directory_echo_mirror_t<internal_t>::ack_version(mailbox_t<void(peer_id_t, directory_echo_version_t)>::address_t peer, directory_echo_version_t version, auto_drainer_t::lock_t) {
    send(mailbox_manager, peer, mailbox_manager->get_connectivity_service()->get_me(), version);
}

#endif   /* CLUSTERING_REACTOR_DIRECTORY_ECHO_TCC_ */
