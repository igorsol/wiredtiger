/*-
 * Copyright (c) 2014-2020 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __key_return --
 *     Change the cursor to reference an internal return key.
 */
static inline int
__key_return(WT_CURSOR_BTREE *cbt)
{
    WT_CURSOR *cursor;
    WT_ITEM *tmp;
    WT_PAGE *page;
    WT_ROW *rip;
    WT_SESSION_IMPL *session;

    page = cbt->ref->page;
    cursor = &cbt->iface;
    session = (WT_SESSION_IMPL *)cbt->iface.session;

    if (page->type == WT_PAGE_ROW_LEAF) {
        rip = &page->pg_row[cbt->slot];

        /*
         * If the cursor references a WT_INSERT item, take its key. Else, if we have an exact match,
         * we copied the key in the search function, take it from there. If we don't have an exact
         * match, take the key from the original page.
         */
        if (cbt->ins != NULL) {
            cursor->key.data = WT_INSERT_KEY(cbt->ins);
            cursor->key.size = WT_INSERT_KEY_SIZE(cbt->ins);
            return (0);
        }

        if (cbt->compare == 0) {
            /*
             * If not in an insert list and there's an exact match, the row-store search function
             * built the key we want to return in the cursor's temporary buffer. Swap the cursor's
             * search-key and temporary buffers so we can return it (it's unsafe to return the
             * temporary buffer itself because our caller might do another search in this table
             * using the key we return, and we'd corrupt the search key during any subsequent search
             * that used the temporary buffer).
             */
            tmp = cbt->row_key;
            cbt->row_key = cbt->tmp;
            cbt->tmp = tmp;

            cursor->key.data = cbt->row_key->data;
            cursor->key.size = cbt->row_key->size;
            return (0);
        }
        return (__wt_row_leaf_key(session, page, rip, &cursor->key, false));
    }

    /*
     * WT_PAGE_COL_FIX, WT_PAGE_COL_VAR:
     *	The interface cursor's record has usually been set, but that
     * isn't universally true, specifically, cursor.search_near may call
     * here without first setting the interface cursor.
     */
    cursor->recno = cbt->recno;
    return (0);
}

/*
 * __time_pairs_init --
 *     Initialize the time pairs to globally visible.
 */
static inline void
__time_pairs_init(WT_TIME_PAIR *start, WT_TIME_PAIR *stop)
{
    start->txnid = WT_TXN_NONE;
    start->timestamp = WT_TS_NONE;
    stop->txnid = WT_TXN_MAX;
    stop->timestamp = WT_TS_MAX;
}

/*
 * __time_pairs_set --
 *     Set the time pairs.
 */
static inline void
__time_pairs_set(WT_TIME_PAIR *start, WT_TIME_PAIR *stop, WT_CELL_UNPACK *unpack)
{
    start->timestamp = unpack->start_ts;
    start->txnid = unpack->start_txn;
    stop->timestamp = unpack->stop_ts;
    stop->txnid = unpack->stop_txn;
}

/*
 * __wt_read_cell_time_pairs --
 *     Read the time pairs from the cell.
 */
void
__wt_read_cell_time_pairs(
  WT_CURSOR_BTREE *cbt, WT_REF *ref, WT_TIME_PAIR *start, WT_TIME_PAIR *stop)
{
    WT_PAGE *page;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)cbt->iface.session;
    page = ref->page;

    WT_ASSERT(session, start != NULL && stop != NULL);

    /* Take the value from the original page cell. */
    if (page->type == WT_PAGE_ROW_LEAF) {
        __wt_read_row_time_pairs(session, page, &page->pg_row[cbt->slot], start, stop);
    } else if (page->type == WT_PAGE_COL_VAR) {
        __wt_read_col_time_pairs(
          session, page, WT_COL_PTR(page, &page->pg_var[cbt->slot]), start, stop);
    } else {
        /* WT_PAGE_COL_FIX: return the default time pairs. */
        __time_pairs_init(start, stop);
    }
}

/*
 * __wt_read_col_time_pairs --
 *     Retrieve the time pairs from a column store cell.
 */
void
__wt_read_col_time_pairs(
  WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL *cell, WT_TIME_PAIR *start, WT_TIME_PAIR *stop)
{
    WT_CELL_UNPACK unpack;

    __wt_cell_unpack(session, page, cell, &unpack);
    __time_pairs_set(start, stop, &unpack);
}

/*
 * __wt_read_row_time_pairs --
 *     Retrieve the time pairs from a row.
 */
void
__wt_read_row_time_pairs(
  WT_SESSION_IMPL *session, WT_PAGE *page, WT_ROW *rip, WT_TIME_PAIR *start, WT_TIME_PAIR *stop)
{
    WT_CELL_UNPACK unpack;

    __time_pairs_init(start, stop);
    /*
     * If a value is simple and is globally visible at the time of reading a page into cache, we set
     * the time pairs as globally visible.
     */
    if (__wt_row_leaf_value_exists(rip))
        return;

    __wt_row_leaf_value_cell(session, page, rip, NULL, &unpack);
    __time_pairs_set(start, stop, &unpack);
}

/*
 * __wt_value_return_buf --
 *     Change a buffer to reference an internal original-page return value.
 */
int
__wt_value_return_buf(
  WT_CURSOR_BTREE *cbt, WT_REF *ref, WT_ITEM *buf, WT_TIME_PAIR *start, WT_TIME_PAIR *stop)
{
    WT_BTREE *btree;
    WT_CELL *cell;
    WT_CELL_UNPACK unpack;
    WT_CURSOR *cursor;
    WT_PAGE *page;
    WT_ROW *rip;
    WT_SESSION_IMPL *session;
    uint8_t v;

    session = (WT_SESSION_IMPL *)cbt->iface.session;
    btree = S2BT(session);

    page = ref->page;
    cursor = &cbt->iface;

    if (start != NULL && stop != NULL)
        __time_pairs_init(start, stop);

    /* Must provide either both start and stop as output parameters or neither. */
    WT_ASSERT(session, (start != NULL && stop != NULL) || (start == NULL && stop == NULL));

    if (page->type == WT_PAGE_ROW_LEAF) {
        rip = &page->pg_row[cbt->slot];

        /*
         * If a value is simple and is globally visible at the time of reading a page into cache, we
         * encode its location into the WT_ROW.
         */
        if (__wt_row_leaf_value(page, rip, buf))
            return (0);

        /* Take the value from the original page cell. */
        __wt_row_leaf_value_cell(session, page, rip, NULL, &unpack);
        if (start != NULL && stop != NULL)
            __time_pairs_set(start, stop, &unpack);

        return (__wt_page_cell_data_ref(session, page, &unpack, buf));
    }

    if (page->type == WT_PAGE_COL_VAR) {
        /* Take the value from the original page cell. */
        cell = WT_COL_PTR(page, &page->pg_var[cbt->slot]);
        __wt_cell_unpack(session, page, cell, &unpack);
        if (start != NULL && stop != NULL)
            __time_pairs_set(start, stop, &unpack);

        return (__wt_page_cell_data_ref(session, page, &unpack, buf));
    }

    /*
     * WT_PAGE_COL_FIX: Take the value from the original page.
     *
     * FIXME-PM-1523: Should also check visibility here
     */
    v = __bit_getv_recno(ref, cursor->recno, btree->bitcnt);
    return (__wt_buf_set(session, buf, &v, 1));
}

/*
 * __value_return --
 *     Change the cursor to reference an internal original-page return value.
 */
static inline int
__value_return(WT_CURSOR_BTREE *cbt)
{
    return (__wt_value_return_buf(cbt, cbt->ref, &cbt->iface.value, NULL, NULL));
}

/*
 * __wt_value_return_upd --
 *     Change the cursor to reference an internal update structure return value.
 */
void
__wt_value_return_upd(WT_CURSOR_BTREE *cbt, WT_UPDATE_VIEW *upd_view)
{
    WT_CURSOR *cursor;
    WT_SESSION_IMPL *session;

    cursor = &cbt->iface;
    session = (WT_SESSION_IMPL *)cbt->iface.session;

    /*
     * We're passed a "standard" or "modified" update that's visible to us. Our caller should have
     * already checked for deleted items (we're too far down the call stack to return not-found).
     *
     * Fast path if it's a standard item, assert our caller's behavior.
     */
    WT_ASSERT(session, upd_view->type == WT_UPDATE_STANDARD);
    /* Ownership should get transferred as appropriate. */
    cursor->value = upd_view->buf;
}

/*
 * __wt_key_return --
 *     Change the cursor to reference an internal return key.
 */
int
__wt_key_return(WT_CURSOR_BTREE *cbt)
{
    WT_CURSOR *cursor;

    cursor = &cbt->iface;

    /*
     * We may already have an internal key and the cursor may not be set up to get another copy, so
     * we have to leave it alone. Consider a cursor search followed by an update: the update doesn't
     * repeat the search, it simply updates the currently referenced key's value. We will end up
     * here with the correct internal key, but we can't "return" the key again even if we wanted to
     * do the additional work, the cursor isn't set up for that because we didn't just complete a
     * search.
     */
    F_CLR(cursor, WT_CURSTD_KEY_EXT);
    if (!F_ISSET(cursor, WT_CURSTD_KEY_INT)) {
        WT_RET(__key_return(cbt));
        F_SET(cursor, WT_CURSTD_KEY_INT);
    }
    return (0);
}

/*
 * __wt_value_return --
 *     Change the cursor to reference an internal return value.
 */
int
__wt_value_return(WT_CURSOR_BTREE *cbt, WT_UPDATE_VIEW *upd_view)
{
    WT_CURSOR *cursor;

    cursor = &cbt->iface;

    F_CLR(cursor, WT_CURSTD_VALUE_EXT);
    if (upd_view->type == WT_UPDATE_INVALID)
        WT_RET(__value_return(cbt));
    else
        __wt_value_return_upd(cbt, upd_view);
    F_SET(cursor, WT_CURSTD_VALUE_INT);
    return (0);
}
