/***
 * Here are the wrappers for the lightning database
 * NOTE: In this implementation, the database will contain the base32 encoded value
 * of the multihash key if the file exists on disk.
 */

#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#include "lmdb.h"
#include "libp2p/utils/logger.h"
#include "libp2p/crypto/encoding/base58.h"
#include "libp2p/os/utils.h"
#include "libp2p/db/datastore.h"
#include "ipfs/repo/fsrepo/lmdb_datastore.h"
#include "ipfs/repo/fsrepo/journalstore.h"
#include "libp2p/db/datastore.h"
#include "varint.h"

/**
 * Build a "value" section for a datastore record
 * @param record the data
 * @param result the data (usually a base32 of the cid hash) + the timestamp as varint
 * @param result_size the size of the result
 * @returns true(1) on success, otherwise 0
 */
int repo_fsrepo_lmdb_encode_record(struct DatastoreRecord* record, uint8_t **result, size_t *result_size) {
	// turn timestamp into varint
	uint8_t ts_varint[8];
	size_t num_bytes;
	if (varint_encode(record->timestamp, &ts_varint[0], 8, &num_bytes) == NULL) {
		return 0;
	}
	// make new structure
	*result = (uint8_t *) malloc(num_bytes + record->value_size);
	if (*result == NULL) {
		return 0;
	}
	memcpy(*result, ts_varint, num_bytes);
	memcpy(&(*result)[num_bytes], record->value, record->value_size);
	*result_size = record->value_size + num_bytes;
	return 1;
}

/**
 * turn lmdb components into a DatastoreRecord structure
 * @param key the key that we searched for in the database
 * @param value the result of the search
 * @param record the complete structure
 * @returns true(1) on success, false(0) otherwise
 */
int repo_fsrepo_lmdb_build_record(MDB_val *key, MDB_val *value, struct DatastoreRecord** record) {
	*record = libp2p_datastore_record_new();
	if (*record != NULL) {
		size_t varint_size = 0;
		struct DatastoreRecord *rec = *record;
		// set key
		rec->key_size = key->mv_size;
		rec->key = (uint8_t *) malloc(rec->key_size);
		if (rec->key == NULL) {
			libp2p_datastore_record_free(*record);
			*record = NULL;
			return 0;
		}
		memcpy(rec->key, key->mv_data, key->mv_size);
		// set value
		rec->timestamp = varint_decode(value->mv_data, value->mv_size, &varint_size);
		rec->value_size = value->mv_size - varint_size;
		rec->value = (uint8_t *) malloc(rec->value_size);
		if (rec->value == NULL) {
			libp2p_datastore_record_free(*record);
			*record = NULL;
			return 0;
		}
		uint8_t *val = (uint8_t*) value->mv_data;
		memcpy(rec->value, &val[varint_size], rec->value_size);
	}
	return 1;
}

/***
 * retrieve a record from the database and put in a pre-sized buffer
 * using an already allocated transaction, and with an already opened
 * database
 * @param key the key to look for
 * @param key_size the length of the key
 * @param record where to put the results
 * @param datastore where to look for the data
 * @param mdb_txn the already opened db transaction
 * @param datastore_table the reference to the already opened datastore table (database)
 * @returns true(1) on success
 */
int repo_fsrepo_lmdb_get_with_transaction(const unsigned char* key, size_t key_size, struct DatastoreRecord** record, MDB_txn *mdb_txn, MDB_dbi *datastore_table) {
	struct MDB_val db_key;
	struct MDB_val db_value;

	// check parameters passed in
	if (mdb_txn == NULL || datastore_table == NULL) {
		libp2p_logger_error("lmdb_datastore", "get_w_tx: invalid transaction or table reference.\n");
		return 0;
	}

	// prepare data
	db_key.mv_size = key_size;
	db_key.mv_data = (char*)key;

	if (mdb_get(mdb_txn, *datastore_table, &db_key, &db_value) != 0) {
		return 0;
	}

	if (!repo_fsrepo_lmdb_build_record(&db_key, &db_value, record)) {
		return 0;
	}

	return 1;

}

/***
 * retrieve a record from the database and put in a pre-sized buffer
 * @param key the key to look for
 * @param key_size the length of the key
 * @param record where to put the results
 * @param datastore where to look for the data
 * @returns true(1) on success
 */
int repo_fsrepo_lmdb_get(const unsigned char* key, size_t key_size, struct DatastoreRecord **record, const struct Datastore* datastore) {
	MDB_txn* mdb_txn;
	MDB_dbi mdb_dbi;

	MDB_env* mdb_env = (MDB_env*)datastore->datastore_handle;
	if (mdb_env == NULL)
		return 0;

	// open transaction
	if (mdb_txn_begin(mdb_env, NULL, 0, &mdb_txn) != 0)
		return 0;

	if (mdb_dbi_open(mdb_txn, "DATASTORE", MDB_DUPSORT | MDB_CREATE, &mdb_dbi) != 0) {
		mdb_txn_commit(mdb_txn);
		return 0;
	}

	int retVal = repo_fsrepo_lmdb_get_with_transaction(key, key_size, record, mdb_txn, &mdb_dbi);

	mdb_txn_commit(mdb_txn);

	return retVal;
}

/**
 * Open the database and create a new transaction
 * @param mdb_env the database handle
 * @param mdb_dbi  the table handle to be created
 * @param mdb_txn the transaction to be created
 * @returns true(1) on success, false(0) otherwise
 */
int lmdb_datastore_create_transaction(MDB_env *mdb_env, MDB_dbi *mdb_dbi, MDB_txn **mdb_txn) {
	// open transaction
	if (mdb_txn_begin(mdb_env, NULL, 0, mdb_txn) != 0)
		return 0;
	if (mdb_dbi_open(*mdb_txn, "DATASTORE", MDB_DUPSORT | MDB_CREATE, mdb_dbi) != 0) {
		mdb_txn_commit(*mdb_txn);
		return 0;
	}
	return 1;
}

/**
 * Write (or update) data in the datastore with the specified key
 * @param key the key
 * @param key_size the length of the key
 * @param data the data to be written
 * @param data_size the length of the data to be written
 * @param datastore the datastore to write to
 * @returns true(1) on success
 */
int repo_fsrepo_lmdb_put(unsigned const char* key, size_t key_size, unsigned char* data, size_t data_size, const struct Datastore* datastore) {
	int retVal;
	MDB_txn *datastore_txn;
	MDB_dbi datastore_table;
	struct MDB_val datastore_key;
	struct MDB_val datastore_value;
	struct DatastoreRecord *datastore_record = NULL;
	struct JournalRecord *journalstore_record = NULL;
	struct lmdb_trans_cursor *journalstore_cursor = NULL;

	MDB_env* mdb_env = (MDB_env*)datastore->datastore_handle;
	if (mdb_env == NULL) {
		libp2p_logger_error("lmdb_datastore", "put: invalid datastore handle.\n");
		return 0;
	}

	// open a transaction to the databases
	if (!lmdb_datastore_create_transaction(mdb_env, &datastore_table, &datastore_txn)) {
		return 0;
	}

	// build the journalstore connectivity stuff
	journalstore_cursor = lmdb_trans_cursor_new();
	if (journalstore_cursor == NULL) {
		libp2p_logger_error("lmdb_datastore", "put: Unable to allocate memory for journalstore cursor.\n");
		return 0;
	}
	journalstore_cursor->environment = mdb_env;
	journalstore_cursor->parent_transaction = datastore_txn;

	// see if what we want is already in the datastore
	repo_fsrepo_lmdb_get_with_transaction(key, key_size, &datastore_record, datastore_txn, &datastore_table);
	if (datastore_record != NULL) {
		// build the journalstore_record with the search criteria
		journalstore_record = lmdb_journal_record_new();
		journalstore_record->hash_size = key_size;
		journalstore_record->hash = malloc(key_size);
		memcpy(journalstore_record->hash, key, key_size);
		journalstore_record->timestamp = datastore_record->timestamp;
		// look up the corresponding journalstore record for possible updating
		lmdb_journalstore_get_record(datastore->datastore_handle, journalstore_cursor, &journalstore_record);
		lmdb_journalstore_cursor_close(journalstore_cursor);
	} else { // it wasn't previously in the database
		datastore_record = libp2p_datastore_record_new();
		if (datastore_record == NULL) {
			libp2p_logger_error("lmdb_datastore", "put: Unable to allocate memory for DatastoreRecord.\n");
			lmdb_trans_cursor_free(journalstore_cursor);
			mdb_txn_commit(datastore_txn);
			return 0;
		}
	}

	// Put in the timestamp if it isn't there already (or is newer)
	unsigned long long now = os_utils_gmtime();
	if (datastore_record->timestamp == 0 || datastore_record->timestamp > now) {
		//we need to update the timestamp. Be sure to update the journal too. (done further down)
		//old_timestamp = datastore_record->timestamp;
		datastore_record->timestamp = now;
	}
	// fill in the other fields
	datastore_record->key_size = key_size;
	datastore_record->key = (uint8_t*) malloc(key_size);
	memcpy(datastore_record->key, key, key_size);
	datastore_record->value_size = data_size;
	datastore_record->value = (uint8_t *) malloc(data_size);
	memcpy(datastore_record->value, data, data_size);

	// convert it into a byte array

	size_t record_size = 0;
	uint8_t *record;
	repo_fsrepo_lmdb_encode_record(datastore_record, &record, &record_size);

	// prepare data
	datastore_key.mv_size = key_size;
	datastore_key.mv_data = (char*)key;

	// write
	datastore_value.mv_size = record_size;
	datastore_value.mv_data = record;

	retVal = mdb_put(datastore_txn, datastore_table, &datastore_key, &datastore_value, MDB_NODUPDATA);

	if (retVal == 0) {
		// Successfully added the datastore record. Now work with the journalstore.
		if (journalstore_record != NULL) {
			if (journalstore_record->timestamp != datastore_record->timestamp) {
				// we need to update
				journalstore_record->timestamp = datastore_record->timestamp;
				lmdb_journalstore_cursor_put(journalstore_cursor, journalstore_record);
				lmdb_journalstore_cursor_close(journalstore_cursor);
				lmdb_journal_record_free(journalstore_record);
			}
		} else {
			// add it to the journalstore
			journalstore_record = lmdb_journal_record_new();
			journalstore_record->hash = (uint8_t*) malloc(key_size);
			memcpy(journalstore_record->hash, key, key_size);
			journalstore_record->hash_size = key_size;
			journalstore_record->timestamp = datastore_record->timestamp;
			journalstore_record->pending = 1; // TODO: Calculate this correctly
			journalstore_record->pin = 1;
			if (!lmdb_journalstore_journal_add(journalstore_cursor, journalstore_record)) {
				libp2p_logger_error("lmdb_datastore", "Datastore record was added, but problem adding Journalstore record. Continuing.\n");
			}
			lmdb_journalstore_cursor_close(journalstore_cursor);
			lmdb_journal_record_free(journalstore_record);
			retVal = 1;
		}
	} else {
		// datastore record was unable to be added.
		libp2p_logger_error("lmdb_datastore", "mdb_put returned %d.\n", retVal);
		retVal = 0;
	}

	// cleanup
	mdb_txn_commit(datastore_txn);
	free(record);
	lmdb_trans_cursor_free(journalstore_cursor);
	libp2p_datastore_record_free(datastore_record);
	return retVal;
}

/**
 * Open an lmdb database with the given parameters.
 * Note: for now, the parameters are not used
 * @param argc number of parameters in the following array
 * @param argv an array of parameters
 */
int repo_fsrepro_lmdb_open(int argc, char** argv, struct Datastore* datastore) {
	// create environment
	struct MDB_env* mdb_env;
	if (mdb_env_create(&mdb_env) < 0) {
		mdb_env_close(mdb_env);
		return 0;
	}

	// at most, 2 databases will be opened. The datastore and the journal.
	MDB_dbi dbs = 2;
	if (mdb_env_set_maxdbs(mdb_env, dbs) != 0) {
		mdb_env_close(mdb_env);
		return 0;
	}

	// open the environment
	if (mdb_env_open(mdb_env, datastore->path, 0, S_IRWXU) < 0) {
		mdb_env_close(mdb_env);
		return 0;
	}

	datastore->datastore_handle = (void*)mdb_env;
	return 1;
}

/***
 * Close an LMDB database
 * NOTE: for now, argc and argv are not used
 * @param argc number of parameters in the argv array
 * @param argv parameters to be passed in
 * @param datastore the datastore struct that contains information about the opened database
 */
int repo_fsrepo_lmdb_close(struct Datastore* datastore) {
	struct MDB_env* mdb_env = (struct MDB_env*)datastore->datastore_handle;
	mdb_env_close(mdb_env);
	return 1;
}

/***
 * Places the LMDB methods into the datastore's function pointers
 * @param datastore the datastore to fill
 * @returns true(1) on success;
 */
int repo_fsrepo_lmdb_cast(struct Datastore* datastore) {
	datastore->datastore_open = &repo_fsrepro_lmdb_open;
	datastore->datastore_close = &repo_fsrepo_lmdb_close;
	datastore->datastore_put = &repo_fsrepo_lmdb_put;
	datastore->datastore_get = &repo_fsrepo_lmdb_get;
	return 1;
}

/***
 * Creates the directory
 * @param datastore contains the path that needs to be created
 * @returns true(1) on success
 */
int repo_fsrepo_lmdb_create_directory(struct Datastore* datastore) {
#ifdef __MINGW32__
	return mkdir(datastore->path) == 0;
#else
	return mkdir(datastore->path, S_IRWXU) == 0;
#endif
}

