/* Masstree
 * Eddie Kohler, Yandong Mao, Robert Morris
 * Copyright (c) 2012-2013 President and Fellows of Harvard College
 * Copyright (c) 2012-2013 Massachusetts Institute of Technology
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Masstree LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Masstree LICENSE file; the license in that file
 * is legally binding.
 */
#ifndef KVROW_HH
#define KVROW_HH 1
#include "vec.hh"
#include "kvio.hh"
#include "kvthread.hh"
#include "kvproto.hh"
#include "log.hh"
#include "json.hh"
#include <algorithm>

template <typename IDX>
struct valueindex {
    static inline IDX make_full() {
        return IDX(0);
    }
    static inline IDX make_fixed(int index, int width) {
        (void) width;
        return IDX(index);
    }
};

template <typename IDX>
struct row_base {
    typedef IDX index_type;
    struct cell_type {
        index_type c_fid;
        Str c_value;
	friend bool operator<(const cell_type& a, const cell_type& b) {
	    return a.c_fid < b.c_fid;
	}
    };
    typedef KUtil::vec<cell_type> change_type;
    typedef KUtil::vec<index_type> fields_type;
    static void sort(change_type& c) {
	std::sort(c.begin(), c.end());
    }
    static void sort(fields_type& f) {
	std::sort(f.begin(), f.end());
    }
    static int kvwrite_change(struct kvout* kvout, const change_type& c) {
        short n = c.size();
	for (short i = 1; i < n; i++)
	    if (!(c[i - 1] < c[i])) {
	        assert(0 && "The change must be sorted");
		exit(EXIT_FAILURE);
	    }
        for (short i = 0; i < n; i++) {
            KVW(kvout, c[i].c_fid);
            KVW(kvout, c[i].c_value);
        }
        return 0;
    }

    static cell_type make_cell(index_type fid, Str value) {
	cell_type c;
	c.c_fid = fid;
	c.c_value = value;
	return c;
    }

    /** @brief Interfaces for column-less key/value store. */
    static void make_get1_fields(fields_type& f) {
        f.resize(1);
        f[0] = valueindex<index_type>::make_full();
    }
    static void make_put1_change(change_type& c, Str val) {
        c.resize(1);
        c[0].c_fid = valueindex<index_type>::make_full();
        c[0].c_value = val;
    }
    static Str make_put_col_request(struct kvout *kvout,
				    index_type fid, Str value) {
	kvout_reset(kvout);
	KVW(kvout, short(1));
	KVW(kvout, fid);
	KVW(kvout, value);
	return Str(kvout->buf, kvout->n);
    }
};


template <typename R>
struct query_helper {
    inline const R* snapshot(const R* row, const typename R::fields_type&, threadinfo&) {
        return row;
    }
};

template <typename R> class query_json_scanner;

template <typename R>
class query {
  public:
    typedef lcdf::Json Json;

    template <typename T>
    void run_get(T& table, Json& req, threadinfo& ti);
    template <typename T>
    bool run_get1(T& table, Str key, int col, Str& value, threadinfo& ti);

    template <typename T>
    result_t run_put(T& table, Str key, Str req, threadinfo& ti);
    template <typename T>
    result_t run_replace(T& table, Str key, Str value, threadinfo& ti);
    template <typename T>
    bool run_remove(T& table, Str key, threadinfo& ti);

    template <typename T>
    void run_scan(T& table, Json& request, threadinfo& ti);
    template <typename T>
    void run_scan1(T& table, Json& request, threadinfo& ti);
    template <typename T>
    void run_rscan1(T& table, Json& request, threadinfo& ti);

    const loginfo::query_times& query_times() const {
        return qtimes_;
    }

  private:
    typename R::fields_type f_;
    loginfo::query_times qtimes_;
    query_helper<R> helper_;
    lcdf::String scankey_;
    int scankeypos_;

    void emit_fields(const R* value, Json& req, threadinfo& ti);
    void emit_fields1(const R* value, Json& req, threadinfo& ti);
    void assign_timestamp(threadinfo& ti);
    void assign_timestamp(threadinfo& ti, kvtimestamp_t t);
    inline bool apply_put(R*& value, bool found, Str req, threadinfo& ti);
    inline bool apply_replace(R*& value, bool found, Str new_value,
                              threadinfo& ti);
    inline void apply_remove(R*& value, kvtimestamp_t& node_ts, threadinfo& ti);

    template <typename RR> friend class query_json_scanner;
};


template <typename R>
void query<R>::emit_fields(const R* value, Json& req, threadinfo& ti) {
    const R* snapshot = helper_.snapshot(value, f_, ti);
    if (f_.empty()) {
        for (int i = 0; i != snapshot->ncol(); ++i)
            req.push_back(lcdf::String::make_stable(snapshot->col(i)));
    } else {
        for (int i = 0; i != (int) f_.size(); ++i)
            req.push_back(lcdf::String::make_stable(snapshot->col(f_[i])));
    }
}

template <typename R>
void query<R>::emit_fields1(const R* value, Json& req, threadinfo& ti) {
    const R* snapshot = helper_.snapshot(value, f_, ti);
    if ((f_.empty() && snapshot->ncol() == 1) || f_.size() == 1)
        req = lcdf::String::make_stable(snapshot->col(f_.empty() ? 0 : f_[0]));
    else if (f_.empty()) {
        for (int i = 0; i != snapshot->ncol(); ++i)
            req.push_back(lcdf::String::make_stable(snapshot->col(i)));
    } else {
        for (int i = 0; i != (int) f_.size(); ++i)
            req.push_back(lcdf::String::make_stable(snapshot->col(f_[i])));
    }
}


template <typename R> template <typename T>
void query<R>::run_get(T& table, Json& req, threadinfo& ti) {
    typename T::unlocked_cursor_type lp(table, req[2].as_s());
    bool found = lp.find_unlocked(ti);
    if (found && row_is_marker(lp.value()))
        found = false;
    if (found) {
        f_.clear();
        for (int i = 3; i != req.size(); ++i)
            f_.push_back(req[i].as_i());
        req.resize(2);
        emit_fields(lp.value(), req, ti);
    }
}

template <typename R> template <typename T>
bool query<R>::run_get1(T& table, Str key, int col, Str& value, threadinfo& ti) {
    typename T::unlocked_cursor_type lp(table, key);
    bool found = lp.find_unlocked(ti);
    if (found && row_is_marker(lp.value()))
        found = false;
    if (found)
        value = lp.value()->col(col);
    return found;
}


template <typename R>
inline void query<R>::assign_timestamp(threadinfo& ti) {
    qtimes_.ts = ti.update_timestamp();
    qtimes_.prev_ts = 0;
}

template <typename R>
inline void query<R>::assign_timestamp(threadinfo& ti, kvtimestamp_t min_ts) {
    qtimes_.ts = ti.update_timestamp(min_ts);
    qtimes_.prev_ts = min_ts;
}


template <typename R> template <typename T>
result_t query<R>::run_put(T& table, Str key, Str req, threadinfo& ti) {
    typename T::cursor_type lp(table, key);
    bool found = lp.find_insert(ti);
    if (!found)
        ti.advance_timestamp(lp.node_timestamp());
    bool inserted = apply_put(lp.value(), found, req, ti);
    lp.finish(1, ti);
    return inserted ? Inserted : Updated;
}

template <typename R>
inline bool query<R>::apply_put(R*& value, bool found, Str req,
                                threadinfo& ti) {
    serial_changeset<typename R::index_type> changeset(req);

    if (loginfo* log = ti.ti_log) {
	log->acquire();
	qtimes_.epoch = global_log_epoch;
    }

    if (!found) {
    insert:
	assign_timestamp(ti);
        value = R::create(changeset, qtimes_.ts, ti);
        return true;
    }

    R* old_value = value;
    assign_timestamp(ti, old_value->timestamp());
    if (row_is_marker(old_value)) {
	old_value->deallocate_rcu(ti);
	goto insert;
    }

    R* updated = old_value->update(changeset, qtimes_.ts, ti);
    if (updated != old_value) {
	value = updated;
	old_value->deallocate_rcu_after_update(changeset, ti);
    }
    return false;
}

template <typename R> template <typename T>
result_t query<R>::run_replace(T& table, Str key, Str value, threadinfo& ti) {
    typename T::cursor_type lp(table, key);
    bool found = lp.find_insert(ti);
    if (!found)
        ti.advance_timestamp(lp.node_timestamp());
    bool inserted = apply_replace(lp.value(), found, value, ti);
    lp.finish(1, ti);
    return inserted ? Inserted : Updated;
}

template <typename R>
inline bool query<R>::apply_replace(R*& value, bool found, Str new_value,
                                    threadinfo& ti) {
    if (loginfo* log = ti.ti_log) {
	log->acquire();
	qtimes_.epoch = global_log_epoch;
    }

    bool inserted = !found || row_is_marker(value);
    if (!found)
	assign_timestamp(ti);
    else {
        assign_timestamp(ti, value->timestamp());
        value->deallocate_rcu(ti);
    }

    value = R::create1(new_value, qtimes_.ts, ti);
    return inserted;
}

template <typename R> template <typename T>
bool query<R>::run_remove(T& table, Str key, threadinfo& ti) {
    typename T::cursor_type lp(table, key);
    bool found = lp.find_locked(ti);
    if (found)
        apply_remove(lp.value(), lp.node_timestamp(), ti);
    lp.finish(-1, ti);
    return found;
}

template <typename R>
inline void query<R>::apply_remove(R*& value, kvtimestamp_t& node_ts,
                                   threadinfo& ti) {
    if (loginfo* log = ti.ti_log) {
	log->acquire();
	qtimes_.epoch = global_log_epoch;
    }

    R* old_value = value;
    assign_timestamp(ti, old_value->timestamp());
    if (circular_int<kvtimestamp_t>::less_equal(node_ts, qtimes_.ts))
	node_ts = qtimes_.ts + 2;
    old_value->deallocate_rcu(ti);
}


template <typename R>
class query_json_scanner {
  public:
    query_json_scanner(query<R> &q, lcdf::Json& request)
	: q_(q), nleft_(request[3].as_i()), request_(request) {
        request_.resize(2);
        q_.scankeypos_ = 0;
    }
    template <typename SS, typename K>
    void visit_leaf(const SS&, const K&, threadinfo&) {
    }
    bool visit_value(Str key, R* value, threadinfo& ti) {
        if (row_is_marker(value))
            return true;
        // NB the `key` is not stable! We must save space for it.
        while (q_.scankeypos_ + key.length() > q_.scankey_.length()) {
            q_.scankey_ = lcdf::String::make_uninitialized(q_.scankey_.length() ? q_.scankey_.length() * 2 : 1024);
            q_.scankeypos_ = 0;
        }
        memcpy(const_cast<char*>(q_.scankey_.data() + q_.scankeypos_),
               key.data(), key.length());
        request_.push_back(q_.scankey_.substring(q_.scankeypos_, key.length()));
        q_.scankeypos_ += key.length();
        request_.push_back(lcdf::Json());
        q_.emit_fields1(value, request_.back(), ti);
        --nleft_;
        return nleft_ != 0;
    }
  private:
    query<R> &q_;
    int nleft_;
    lcdf::Json& request_;
};

template <typename R> template <typename T>
void query<R>::run_scan(T& table, Json& request, threadinfo& ti) {
    assert(request[3].as_i() > 0);
    lcdf::Str key = request[2].as_s();
    f_.clear();
    for (int i = 4; i != request.size(); ++i)
        f_.push_back(request[i].as_i());
    query_json_scanner<R> scanf(*this, request);
    table.scan(key, true, scanf, ti);
}

template <typename R> template <typename T>
void query<R>::run_scan1(T& table, Json& request, threadinfo& ti) {
    assert(request[3].as_i() > 0);
    lcdf::Str key = request[2].as_s();
    R::make_get1_fields(f_);
    query_json_scanner<R> scanf(*this, request);
    table.scan(key, true, scanf, ti);
}

template <typename R> template <typename T>
void query<R>::run_rscan1(T& table, Json& request, threadinfo& ti) {
    assert(request[3].as_i() > 0);
    lcdf::Str key = request[2].as_s();
    R::make_get1_fields(f_);
    query_json_scanner<R> scanf(*this, request);
    table.rscan(key, true, scanf, ti);
}

#include KVDB_ROW_TYPE_INCLUDE
#endif
