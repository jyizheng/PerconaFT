/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
/*
COPYING CONDITIONS NOTICE:

  This program is free software; you can redistribute it and/or modify
  it under the terms of version 2 of the GNU General Public License as
  published by the Free Software Foundation, and provided that the
  following conditions are met:

      * Redistributions of source code must retain this COPYING
        CONDITIONS NOTICE, the COPYRIGHT NOTICE (below), the
        DISCLAIMER (below), the UNIVERSITY PATENT NOTICE (below), the
        PATENT MARKING NOTICE (below), and the PATENT RIGHTS
        GRANT (below).

      * Redistributions in binary form must reproduce this COPYING
        CONDITIONS NOTICE, the COPYRIGHT NOTICE (below), the
        DISCLAIMER (below), the UNIVERSITY PATENT NOTICE (below), the
        PATENT MARKING NOTICE (below), and the PATENT RIGHTS
        GRANT (below) in the documentation and/or other materials
        provided with the distribution.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
  02110-1301, USA.

COPYRIGHT NOTICE:

  TokuFT, Tokutek Fractal Tree Indexing Library.
  Copyright (C) 2007-2013 Tokutek, Inc.

DISCLAIMER:

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

UNIVERSITY PATENT NOTICE:

  The technology is licensed by the Massachusetts Institute of
  Technology, Rutgers State University of New Jersey, and the Research
  Foundation of State University of New York at Stony Brook under
  United States of America Serial No. 11/760379 and to the patents
  and/or patent applications resulting from it.

PATENT MARKING NOTICE:

  This software is covered by US Patent No. 8,185,551.
  This software is covered by US Patent No. 8,489,638.

PATENT RIGHTS GRANT:

  "THIS IMPLEMENTATION" means the copyrightable works distributed by
  Tokutek as part of the Fractal Tree project.

  "PATENT CLAIMS" means the claims of patents that are owned or
  licensable by Tokutek, both currently or in the future; and that in
  the absence of this license would be infringed by THIS
  IMPLEMENTATION or by using or running THIS IMPLEMENTATION.

  "PATENT CHALLENGE" shall mean a challenge to the validity,
  patentability, enforceability and/or non-infringement of any of the
  PATENT CLAIMS or otherwise opposing any of the PATENT CLAIMS.

  Tokutek hereby grants to you, for the term and geographical scope of
  the PATENT CLAIMS, a non-exclusive, no-charge, royalty-free,
  irrevocable (except as stated in this section) patent license to
  make, have made, use, offer to sell, sell, import, transfer, and
  otherwise run, modify, and propagate the contents of THIS
  IMPLEMENTATION, where such license applies only to the PATENT
  CLAIMS.  This grant does not include claims that would be infringed
  only as a consequence of further modifications of THIS
  IMPLEMENTATION.  If you or your agent or licensee institute or order
  or agree to the institution of patent litigation against any entity
  (including a cross-claim or counterclaim in a lawsuit) alleging that
  THIS IMPLEMENTATION constitutes direct or contributory patent
  infringement, or inducement of patent infringement, then any rights
  granted to you under this License shall terminate as of the date
  such litigation is filed.  If you or your agent or exclusive
  licensee institute or order or agree to the institution of a PATENT
  CHALLENGE, then Tokutek may terminate any rights granted to you
  under this License.
*/

#ident "Copyright (c) 2007-2013 Tokutek Inc.  All rights reserved."
#ident "The technology is licensed by the Massachusetts Institute of Technology, Rutgers State University of New Jersey, and the Research Foundation of State University of New York at Stony Brook under United States of America Serial No. 11/760379 and to the patents and/or patent applications resulting from it."
#ident "$Id$"

#include <ctype.h>

#include <db.h>
#include "dictionary.h"
#include "ft/ft.h"
#include "ydb-internal.h"
#include "ydb_db.h"
#include "ydb_write.h"
#include "ydb_cursor.h"
#include <locktree/locktree.h>
#include "ydb_row_lock.h"
#include "iname_helpers.h"


// set the descriptor and cmp_descriptor to the
// descriptors from the given ft, updating the
// locktree's descriptor pointer if necessary
static void
db_set_descriptors(DB *db, FT_HANDLE ft_handle) {
    const toku::comparator &cmp = toku_ft_get_comparator(ft_handle);
    db->descriptor = toku_ft_get_descriptor(ft_handle);
    db->cmp_descriptor = toku_ft_get_cmp_descriptor(ft_handle);
    invariant(db->cmp_descriptor == cmp.get_descriptor());
}

static int toku_db_open_iname(DB * db, DB_TXN * txn, const char *iname_in_env, uint32_t flags) {
    //Set comparison functions if not yet set.
    HANDLE_READ_ONLY_TXN(txn);
    // we should always have SOME environment comparison function
    // set, even if it is the default one set in toku_env_create
    invariant(db->dbenv->i->bt_compare);
    bool need_locktree = (bool)((db->dbenv->i->open_flags & DB_INIT_LOCK) &&
                                (db->dbenv->i->open_flags & DB_INIT_TXN));

    int is_db_excl    = flags & DB_EXCL;    flags&=~DB_EXCL;
    int is_db_create  = flags & DB_CREATE;  flags&=~DB_CREATE;
     // unknown or conflicting flags are bad
     if (is_db_excl && !is_db_create) {
        return EINVAL;
    }

    if (db_opened(db)) {
        return EINVAL;              /* It was already open. */
    }
     
    FT_HANDLE ft_handle = db->i->ft_handle;
    int r = toku_ft_handle_open(ft_handle, iname_in_env,
                      is_db_create, is_db_excl,
                      db->dbenv->i->cachetable,
                      txn ? db_txn_struct_i(txn)->tokutxn : nullptr);
    if (r != 0) {
        goto out;
    }

    // if the dictionary was opened as a blackhole, mark the
    // fractal tree as blackhole too.
    if (flags & DB_BLACKHOLE) {
        toku_ft_set_blackhole(ft_handle);
    }

    db->i->opened = 1;

    // now that the handle has successfully opened, a valid descriptor
    // is in the ft. we need to set the db's descriptor pointers
    db_set_descriptors(db, ft_handle);

    if (need_locktree) {
        db->i->dict_id = toku_ft_get_dictionary_id(db->i->ft_handle);
        struct lt_on_create_callback_extra on_create_extra = {
            .txn = txn,
            .ft_handle = db->i->ft_handle,
        };
        db->i->lt = db->dbenv->i->ltm.get_lt(db->i->dict_id,
                                             toku_ft_get_comparator(db->i->ft_handle),
                                             &on_create_extra);
        if (db->i->lt == nullptr) {
            r = errno;
            if (r == 0) {
                r = EINVAL;
            }
            goto out;
        }
    }
    r = 0;
 
out:
    if (r != 0) {
        db->i->dict_id = DICTIONARY_ID_NONE;
        db->i->opened = 0;
        if (db->i->lt) {
            db->dbenv->i->ltm.release_lt(db->i->lt);
            db->i->lt = nullptr;
        }
    }
    return r;
}

void dictionary::create(const char* dname, dictionary_manager* manager) {
    if (dname) {
        m_dname = toku_strdup(dname);
    }
    else {
        m_dname = nullptr;
    }
    m_refcount = 0;
    m_mgr = manager;
}

void dictionary::destroy(){
    invariant(m_refcount == 0);
    if (m_dname) {
        toku_free(m_dname);
    }
}

void dictionary::release(){
    bool do_destroy = false;
    if (m_mgr) {
        do_destroy = m_mgr->release_dictionary(this);
    }
    else {
        invariant(m_refcount == 0);
        do_destroy = true;
    }
    if (do_destroy) {
        destroy();
    }
}

char* dictionary::get_dname() const {
    return m_dname;
}

// verifies that either all of the metadata files we are expecting exist
// or none do.
int dictionary_manager::validate_environment(DB_ENV* env, bool* valid_newenv) {
    int r;
    bool expect_newenv = false;        // set true if we expect to create a new env
    toku_struct_stat buf;
    char* path = NULL;

    // Test for persistent environment
    path = toku_construct_full_name(2, env->i->dir, toku_product_name_strings.environmentdictionary);
    assert(path);
    r = toku_stat(path, &buf);
    if (r == 0) {
        expect_newenv = false;  // persistent info exists
    }
    else {
        int stat_errno = get_error_errno();
        if (stat_errno == ENOENT) {
            expect_newenv = true;
            r = 0;
        }
        else {
            r = toku_ydb_do_error(env, stat_errno, "Unable to access persistent environment\n");
            assert(r);
        }
    }
    toku_free(path);

    // Test for fileops directory
    if (r == 0) {
        path = toku_construct_full_name(2, env->i->dir, toku_product_name_strings.fileopsdirectory);
        assert(path);
        r = toku_stat(path, &buf);
        if (r == 0) {  
            if (expect_newenv)  // fileops directory exists, but persistent env is missing
                r = toku_ydb_do_error(env, ENOENT, "Persistent environment is missing\n");
        }
        else {
            int stat_errno = get_error_errno();
            if (stat_errno == ENOENT) {
                if (!expect_newenv)  // fileops directory is missing but persistent env exists
                    r = toku_ydb_do_error(env, ENOENT, "Fileops directory is missing\n");
                else 
                    r = 0;           // both fileops directory and persistent env are missing
            }
            else {
                r = toku_ydb_do_error(env, stat_errno, "Unable to access fileops directory\n");
                assert(r);
            }
        }
        toku_free(path);
    }

    if (r == 0)
        *valid_newenv = expect_newenv;
    else 
        *valid_newenv = false;
    return r;
}

// Keys used in persistent environment dictionary:
// Following keys added in version 12
static const char * orig_env_ver_key = "original_version";
static const char * curr_env_ver_key = "current_version";  
// Following keys added in version 14, add more keys for future versions
static const char * creation_time_key         = "creation_time";

static char * get_upgrade_time_key(int version) {
    static char upgrade_time_key[sizeof("upgrade_v_time") + 12];
    {
        int n;
        n = snprintf(upgrade_time_key, sizeof(upgrade_time_key), "upgrade_v%d_time", version);
        assert(n >= 0 && n < (int)sizeof(upgrade_time_key));
    }
    return &upgrade_time_key[0];
}

static char * get_upgrade_footprint_key(int version) {
    static char upgrade_footprint_key[sizeof("upgrade_v_footprint") + 12];
    {
        int n;
        n = snprintf(upgrade_footprint_key, sizeof(upgrade_footprint_key), "upgrade_v%d_footprint", version);
        assert(n >= 0 && n < (int)sizeof(upgrade_footprint_key));
    }
    return &upgrade_footprint_key[0];
}

static char * get_upgrade_last_lsn_key(int version) {
    static char upgrade_last_lsn_key[sizeof("upgrade_v_last_lsn") + 12];
    {
        int n;
        n = snprintf(upgrade_last_lsn_key, sizeof(upgrade_last_lsn_key), "upgrade_v%d_last_lsn", version);
        assert(n >= 0 && n < (int)sizeof(upgrade_last_lsn_key));
    }
    return &upgrade_last_lsn_key[0];
}

// Requires: persistent environment dictionary is already open.
// Input arg is lsn of clean shutdown of previous version,
// or ZERO_LSN if no upgrade or if crash between log upgrade and here.
// NOTE: To maintain compatibility with previous versions, do not change the 
//       format of any information stored in the persistent environment dictionary.
//       For example, some values are stored as 32 bits, even though they are immediately
//       converted to 64 bits when read.  Do not change them to be stored as 64 bits.
//
int dictionary_manager::maybe_upgrade_persistent_environment_dictionary(
    DB_TXN * txn,
    LSN last_lsn_of_clean_shutdown_read_from_log
    )
{
    int r;
    DBT key, val;

    toku_fill_dbt(&key, curr_env_ver_key, strlen(curr_env_ver_key));
    toku_init_dbt(&val);
    r = toku_db_get(m_persistent_environment, txn, &key, &val, 0);
    assert(r == 0);
    uint32_t stored_env_version = toku_dtoh32(*(uint32_t*)val.data);
    if (stored_env_version > FT_LAYOUT_VERSION)
        r = TOKUDB_DICTIONARY_TOO_NEW;
    else if (stored_env_version < FT_LAYOUT_MIN_SUPPORTED_VERSION)
        r = TOKUDB_DICTIONARY_TOO_OLD;
    else if (stored_env_version < FT_LAYOUT_VERSION) {
        const uint32_t curr_env_ver_d = toku_htod32(FT_LAYOUT_VERSION);
        toku_fill_dbt(&key, curr_env_ver_key, strlen(curr_env_ver_key));
        toku_fill_dbt(&val, &curr_env_ver_d, sizeof(curr_env_ver_d));
        r = toku_db_put(m_persistent_environment, txn, &key, &val, 0, false);
        assert_zero(r);

        time_t upgrade_time_d = toku_htod64(time(NULL));
        uint64_t upgrade_footprint_d = toku_htod64(toku_log_upgrade_get_footprint());
        uint64_t upgrade_last_lsn_d = toku_htod64(last_lsn_of_clean_shutdown_read_from_log.lsn);
        for (int version = stored_env_version+1; version <= FT_LAYOUT_VERSION; version++) {
            uint32_t put_flag = DB_NOOVERWRITE;
            if (version <= FT_LAYOUT_VERSION_19) {
                // See #5902.
                // To prevent a crash (and any higher complexity code) we'll simply
                // silently not overwrite anything if it exists.
                // The keys existing for version <= 19 is not necessarily an error.
                // If this happens for versions > 19 it IS an error and we'll use DB_NOOVERWRITE.
                put_flag = DB_NOOVERWRITE_NO_ERROR;
            }


            char* upgrade_time_key = get_upgrade_time_key(version);
            toku_fill_dbt(&key, upgrade_time_key, strlen(upgrade_time_key));
            toku_fill_dbt(&val, &upgrade_time_d, sizeof(upgrade_time_d));
            r = toku_db_put(m_persistent_environment, txn, &key, &val, put_flag, false);
            assert_zero(r);

            char* upgrade_footprint_key = get_upgrade_footprint_key(version);
            toku_fill_dbt(&key, upgrade_footprint_key, strlen(upgrade_footprint_key));
            toku_fill_dbt(&val, &upgrade_footprint_d, sizeof(upgrade_footprint_d));
            r = toku_db_put(m_persistent_environment, txn, &key, &val, put_flag, false);
            assert_zero(r);

            char* upgrade_last_lsn_key = get_upgrade_last_lsn_key(version);
            toku_fill_dbt(&key, upgrade_last_lsn_key, strlen(upgrade_last_lsn_key));
            toku_fill_dbt(&val, &upgrade_last_lsn_d, sizeof(upgrade_last_lsn_d));
            r = toku_db_put(m_persistent_environment, txn, &key, &val, put_flag, false);
            assert_zero(r);
        }

    }
    return r;
}

int dictionary_manager::setup_persistent_environment(
    DB_ENV* env,
    bool newenv,
    DB_TXN* txn,
    LSN last_lsn_of_clean_shutdown_read_from_log
    ) 
{
    int r = 0;
    r = toku_db_create(&m_persistent_environment, env, 0);
    assert_zero(r);
    toku_db_use_builtin_key_cmp(m_persistent_environment);
    r = open_internal_db(m_persistent_environment, txn, NULL, toku_product_name_strings.environmentdictionary, DB_CREATE);
    if (r != 0) {
        r = toku_ydb_do_error(env, r, "Cant open persistent env\n");
        goto cleanup;
    }
    if (newenv) {
        // create new persistent_environment
        DBT key, val;
        uint32_t persistent_original_env_version = FT_LAYOUT_VERSION;
        const uint32_t environment_version = toku_htod32(persistent_original_env_version);

        toku_fill_dbt(&key, orig_env_ver_key, strlen(orig_env_ver_key));
        toku_fill_dbt(&val, &environment_version, sizeof(environment_version));
        r = toku_db_put(m_persistent_environment, txn, &key, &val, 0, false);
        assert_zero(r);

        toku_fill_dbt(&key, curr_env_ver_key, strlen(curr_env_ver_key));
        toku_fill_dbt(&val, &environment_version, sizeof(environment_version));
        r = toku_db_put(m_persistent_environment, txn, &key, &val, 0, false);
        assert_zero(r);

        time_t creation_time_d = toku_htod64(time(NULL));
        toku_fill_dbt(&key, creation_time_key, strlen(creation_time_key));
        toku_fill_dbt(&val, &creation_time_d, sizeof(creation_time_d));
        r = toku_db_put(m_persistent_environment, txn, &key, &val, 0, false);
        assert_zero(r);
    }
    else {
        r = maybe_upgrade_persistent_environment_dictionary(txn, last_lsn_of_clean_shutdown_read_from_log);
        assert_zero(r);
    }
cleanup:
    return r;
}

int dictionary_manager::setup_directory(DB_ENV* env, DB_TXN* txn) {
    int r = toku_db_create(&m_directory, env, 0);
    assert_zero(r);
    toku_db_use_builtin_key_cmp(m_directory);
    r = open_internal_db(m_directory, txn, NULL, toku_product_name_strings.fileopsdirectory, DB_CREATE);
    if (r != 0) {
        r = toku_ydb_do_error(env, r, "Cant open %s\n", toku_product_name_strings.fileopsdirectory);
    }
    return r;
}

int dictionary_manager::setup_metadata(
    DB_ENV* env,
    bool newenv,
    DB_TXN* txn,
    LSN last_lsn_of_clean_shutdown_read_from_log
    )
{
    int r = 0;
    r = setup_persistent_environment(
        env,
        newenv,
        txn,
        last_lsn_of_clean_shutdown_read_from_log
        );
    if (r != 0) goto cleanup;
    r = setup_directory(env, txn);
    
cleanup:
    return r;
}


int dictionary_manager::get_persistent_environment_cursor(DB_TXN* txn, DBC** c) {
    return toku_db_cursor(m_persistent_environment, txn, c, 0);
}

int dictionary_manager::get_directory_cursor(DB_TXN* txn, DBC** c) {
    return toku_db_cursor(m_directory, txn, c, 0);
}

// get the iname for the given dname and set it in the variable iname
// responsibility of caller to free iname
int dictionary_manager::get_iname(const char* dname, DB_TXN* txn, char** iname) {
    DBT dname_dbt;
    DBT iname_dbt;
    toku_fill_dbt(&dname_dbt, dname, strlen(dname)+1);
    toku_init_dbt_flags(&iname_dbt, DB_DBT_MALLOC);

    // get iname
    int r = toku_db_get(m_directory, txn, &dname_dbt, &iname_dbt, DB_SERIALIZABLE);  // allocates memory for iname
    if (r == 0) {
        *iname = (char *) iname_dbt.data;
    }
    return r;
}

// pure laziness, should really only need above function
int dictionary_manager::get_iname_in_dbt(DBT* dname_dbt, DBT* iname_dbt) {
    return autotxn_db_get(m_directory, NULL, dname_dbt, iname_dbt, DB_SERIALIZABLE|DB_PRELOCKED); // allocates memory for iname
}

int dictionary_manager::change_iname(DB_TXN* txn, const char* dname, const char* new_iname, uint32_t put_flags) {
    DBT dname_dbt;  // holds dname
    toku_fill_dbt(&dname_dbt, dname, strlen(dname)+1);
    DBT iname_dbt;  // holds new iname
    toku_fill_dbt(&iname_dbt, new_iname, strlen(new_iname) + 1);      // iname_in_env goes in directory
    return toku_db_put(m_directory, txn, &dname_dbt, &iname_dbt, put_flags, true);
}

int dictionary_manager::pre_acquire_fileops_lock(DB_TXN* txn, char* dname) {
    DBT key_in_directory = { .data = dname, .size = (uint32_t) strlen(dname)+1 };
    //Left end of range == right end of range (point lock)
    return toku_db_get_range_lock(m_directory, txn,
            &key_in_directory, &key_in_directory,
            toku::lock_request::type::WRITE);
}

// see if we can acquire a table lock for the given dname.
// requires: write lock on dname in the directory. dictionary
//          open, close, and begin checkpoint cannot occur.
// returns: true if we could open, lock, and close a dictionary
//          with the given dname, false otherwise.
bool
dictionary_manager::can_acquire_table_lock(DB_ENV *env, DB_TXN *txn, const char *iname_in_env) {
    int r;
    bool got_lock = false;
    DB *db;

    r = toku_db_create(&db, env, 0);
    assert_zero(r);
    r = open_internal_db(db, txn, NULL, iname_in_env, 0);
    assert_zero(r);
    r = toku_db_pre_acquire_table_lock(db, txn);
    if (r == 0) {
        got_lock = true;
    } else {
        got_lock = false;
    }
    toku_db_close(db);

    return got_lock;
}

int dictionary_manager::rename(DB_ENV* env, DB_TXN *txn, const char *old_dname, const char *new_dname) {
    // TODO: possibly do an early check here for open handles
    char *iname = NULL;
    char *dummy = NULL; // used to verify an iname does not already exist for new_dname
    int r = get_iname(old_dname, txn, &iname);
    if (r == DB_NOTFOUND) {
        r = ENOENT;
    }
    else if (r == 0) {
        // verify that newname does not already exist
        r = get_iname(new_dname, txn, &dummy);
        if (r == 0) {
            r = EEXIST;
        }
        else if (r == DB_NOTFOUND) {
            // remove old (dname,iname) and insert (newname,iname) in directory
            DBT old_dname_dbt;
            toku_fill_dbt(&old_dname_dbt, old_dname, strlen(old_dname)+1);
            DBT new_dname_dbt;
            toku_fill_dbt(&new_dname_dbt, new_dname, strlen(new_dname)+1);
            DBT iname_dbt;
            toku_fill_dbt(&iname_dbt, iname, strlen(iname)+1);
            r = toku_db_del(m_directory, txn, &old_dname_dbt, DB_DELETE_ANY, true);
            if (r != 0) { goto exit; }
            r = toku_db_put(m_directory, txn, &new_dname_dbt, &iname_dbt, 0, true);
            if (r != 0) { goto exit; }

            //Now that we have writelocks on both dnames, verify that there are still no handles open. (to prevent race conditions)
            dictionary* old_dict = NULL;
            dictionary* new_dict = NULL;
            toku_mutex_lock(&m_mutex);
            old_dict = find(old_dname);
            new_dict = find(new_dname);
            toku_mutex_unlock(&m_mutex);
            
            if (old_dict) {
                printf("Cannot rename dictionary with an open handle.\n");
                r = EINVAL;
                goto exit;
            }
            if (new_dict) {
                printf("Cannot rename dictionary; Dictionary with target name has an open handle.\n");
                r = EINVAL;
                goto exit;
            }

            // we know a live db handle does not exist.
            //
            // use the internally opened db to try and get a table lock
            // 
            // if we can't get it, then some txn needs the ft and we
            // should return lock not granted.
            //
            // otherwise, we're okay in marking this ft as remove on
            // commit. no new handles can open for this dictionary
            // because the txn has directory write locks on the dname
            if (txn && !can_acquire_table_lock(env, txn, iname)) {
                r = DB_LOCK_NOTGRANTED;
            }
        }
    }

exit:
    if (iname) {
        toku_free(iname);
    }
    if (dummy) {
        toku_free(iname);
    }
    return r;
}

int dictionary_manager::remove(const char * dname, DB_ENV* env, DB_TXN* txn) {
    // TODO: perhaps add a fast path of bailing if open handles exist
    char* iname = NULL;
    int r = get_iname(dname, txn, &iname);

    DB *db = NULL;
    if (r != 0) {
        if (r == DB_NOTFOUND) {
            r = ENOENT;
        }
        goto exit;
    }
    // remove (dname,iname) from directory
    DBT dname_dbt;  
    toku_fill_dbt(&dname_dbt, dname, strlen(dname)+1);
    r = toku_db_del(m_directory, txn, &dname_dbt, DB_DELETE_ANY, true);
    if (r != 0) {
        goto exit;
    }
    r = toku_db_create(&db, env, 0);
    lazy_assert_zero(r);
    r = open_internal_db(db, txn, NULL, iname, 0);
    if (txn && r) {
        if (r == EMFILE || r == ENFILE)
            r = toku_ydb_do_error(env, r, "toku dbremove failed because open file limit reached\n");
        else
            r = toku_ydb_do_error(env, r, "toku dbremove failed\n");
        goto exit;
    }
    if (txn) {
        dictionary* old_dict = NULL;
        toku_mutex_lock(&m_mutex);
        old_dict = find(dname);
        toku_mutex_unlock(&m_mutex);
        // Now that we have a writelock on dname, verify that there are still no handles open. (to prevent race conditions)
        if (old_dict) {
            r = toku_ydb_do_error(env, EINVAL, "Cannot remove dictionary with an open handle.\n");
            goto exit;
        }
        // we know a live db handle does not exist.
        //
        // use the internally opened db to try and get a table lock
        //
        // if we can't get it, then some txn needs the ft and we
        // should return lock not granted.
        //
        // otherwise, we're okay in marking this ft as remove on
        // commit. no new handles can open for this dictionary
        // because the txn has directory write locks on the dname
        r = toku_db_pre_acquire_table_lock(db, txn);
        if (r != 0) {
            r = DB_LOCK_NOTGRANTED;
            goto exit;
        }
        // The ft will be unlinked when the txn commits
        toku_ft_unlink_on_commit(db->i->ft_handle, db_txn_struct_i(txn)->tokutxn);
    }
    else {
        // unlink the ft without a txn
        toku_ft_unlink(db->i->ft_handle);
    }

exit:
    if (db) {
        toku_db_close(db);
    }
    if (iname) {
        toku_free(iname);
    }
    return r;
}

int dictionary_manager::open_internal_db(DB* db, DB_TXN* txn, const char* dname, const char* iname, uint32_t flags) {    
    int r = toku_db_open_iname(db, txn, iname, flags);
    if (r == 0) {
        dictionary *dbi = NULL;
        XCALLOC(dbi);
        dbi->create(dname, NULL);
        db->i->dict = dbi;
    }
    return r;
}

int dictionary_manager::open_db(
    DB* db,
    const char * dname,
    DB_TXN * txn,
    uint32_t flags
    )
{
    int r = 0;
    int is_db_excl = flags & DB_EXCL;
    int is_db_create = flags & DB_CREATE;
    int is_db_hot_index  = flags & DB_IS_HOT_INDEX;
    
    assert(!db_opened(db));
    char* iname = NULL;
    r = get_iname(dname, txn, &iname);
    if (r == DB_NOTFOUND && !is_db_create) {
        r = ENOENT;
    } else if (r==0 && is_db_excl) {
        r = EEXIST;
    } else if (r == DB_NOTFOUND) {
        iname = create_new_iname(dname, db->dbenv, txn, NULL);
        uint32_t put_flags = 0 | ((is_db_hot_index) ? DB_PRELOCKED_WRITE : 0); 
        change_iname(txn, dname, iname, put_flags);
        r = 0;
    }
    
    // we now have an iname
    if (r == 0) {
        r = toku_db_open_iname(db, txn, iname, flags);
        if (r == 0) {
            // now that the directory has been updated, create the dictionary
            db->i->dict = get_dictionary(dname);
        }
    }

    if (iname) {
        toku_free(iname);
    }
    return r;
}

void dictionary_manager::create() {
    ZERO_STRUCT(m_mutex);
    toku_mutex_init(&m_mutex, nullptr);
    m_dictionary_map.create();
}

void dictionary_manager::destroy() {
    if (m_persistent_environment) {
        toku_db_close(m_persistent_environment);
    }
    if (m_directory) {
        toku_db_close(m_directory);
    }
    m_dictionary_map.destroy();
    toku_mutex_destroy(&m_mutex);
}

int dictionary_manager::find_by_dname(dictionary *const &dbi, const char* const &dname) {
    return strcmp(dbi->get_dname(), dname);
}

dictionary* dictionary_manager::find(const char* dname) {
    dictionary *dbi;
    int r = m_dictionary_map.find_zero<const char *, find_by_dname>(dname, &dbi, nullptr);
    return r == 0 ? dbi : nullptr;
}

void dictionary_manager::add_db(dictionary* dbi) {
    int r = m_dictionary_map.insert<const char *, find_by_dname>(dbi, dbi->get_dname(), nullptr);
    invariant_zero(r);
}

void dictionary_manager::remove_dictionary(dictionary* dbi) {
    uint32_t idx;
    dictionary *found_dbi;
    const char* dname = dbi->get_dname();
    int r = m_dictionary_map.find_zero<const char *, find_by_dname>(
        dname,
        &found_dbi,
        &idx
        );
    invariant_zero(r);
    invariant(found_dbi == dbi);
    r = m_dictionary_map.delete_at(idx);
    invariant_zero(r);
}

bool dictionary_manager::release_dictionary(dictionary* dbi) {
    bool do_destroy = false;
    toku_mutex_lock(&m_mutex);
    dbi->m_refcount--;
    if (dbi->m_refcount == 0) {
        remove_dictionary(dbi);
        do_destroy = true;
    }
    toku_mutex_unlock(&m_mutex);
    return do_destroy;
}

uint32_t dictionary_manager::num_open_dictionaries() {
    toku_mutex_lock(&m_mutex);
    uint32_t retval =  m_dictionary_map.size();
    toku_mutex_unlock(&m_mutex);
    return retval;    
}

dictionary* dictionary_manager::get_dictionary(const char * dname) {
    toku_mutex_lock(&m_mutex);
    dictionary *dbi = find(dname);
    if (dbi == nullptr) {
        XCALLOC(dbi);
        dbi->create(dname, this);
        add_db(dbi);
    }
    dbi->m_refcount++;
    toku_mutex_unlock(&m_mutex);
    return dbi;
}
