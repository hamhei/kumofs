#include "storage.h"  // FIXME
#include <tchdb.h>
#include <mp/pthread.h>

struct kumo_tchdb {
	kumo_tchdb()
	{
		db = tchdbnew();
		if(!db) {
			throw std::bad_alloc();
		}

		garbage = kumo_buffer_queue_new();
		if(!garbage) {
			tchdbdel(db);
			throw std::bad_alloc();
		}
	}

	~kumo_tchdb()
	{
		tchdbdel(db);
		kumo_buffer_queue_free(garbage);
	}

	TCHDB* db;
	mp::pthread_mutex iterator_mutex;

	kumo_buffer_queue* garbage;
	mp::pthread_mutex garbage_mutex;
};


static void* kumo_tchdb_create(void)
try {
	kumo_tchdb* ctx = new kumo_tchdb();
	return reinterpret_cast<void*>(ctx);

} catch (...) {
	return NULL;
}

static void kumo_tchdb_free(void* data)
{
	kumo_tchdb* ctx = reinterpret_cast<kumo_tchdb*>(data);
	delete ctx;
}

static bool kumo_tchdb_open(void* data, const char* path)
{
	kumo_tchdb* ctx = reinterpret_cast<kumo_tchdb*>(data);

	if(!tchdbsetmutex(ctx->db)) {
		return false;
	}

	if(!tchdbopen(ctx->db, path, HDBOWRITER|HDBOCREAT)) {
		return false;
	}

	return true;
}

static void kumo_tchdb_close(void* data)
{
	kumo_tchdb* ctx = reinterpret_cast<kumo_tchdb*>(data);
	tchdbclose(ctx->db);
}


static const char* kumo_tchdb_get(void* data,
		const char* key, uint32_t keylen,
		uint32_t* result_vallen,
		msgpack_zone* zone)
{
	kumo_tchdb* ctx = reinterpret_cast<kumo_tchdb*>(data);

	int len;
	char* val = (char*)tchdbget(ctx->db, key, keylen, &len);
	if(!val) {
		return NULL;
	}
	*result_vallen = len;

	if(!msgpack_zone_push_finalizer(zone, free, val)) {
		free(val);
		return NULL;
	}

	return val;
}


typedef struct {
	const char* val;
	uint32_t vallen;
} kumo_tchdb_update_ctx;

static void* kumo_tchdb_update_proc(const void* vbuf, int vsiz, int *sp, void* op)
{
	kumo_tchdb_update_ctx* upctx = (kumo_tchdb_update_ctx*)op;

	if(vsiz < 8 || kumo_clocktime_less(
			kumo_storage_clocktime_of((const char*)vbuf),
			kumo_storage_clocktime_of(upctx->val))) {

		void* mem = malloc(upctx->vallen);
		if(!mem) {
			return NULL;  // FIXME
		}

		*sp = upctx->vallen;
		memcpy(mem, upctx->val, upctx->vallen);
		return mem;

	} else {
		return NULL;
	}
}


static bool kumo_tchdb_set(void* data,
		const char* key, uint32_t keylen,
		const char* val, uint32_t vallen)
{
	kumo_tchdb* ctx = reinterpret_cast<kumo_tchdb*>(data);
	return tchdbput(ctx->db, key, keylen, val, vallen);
}

static bool kumo_tchdb_update(void* data,
		const char* key, uint32_t keylen,
		const char* val, uint32_t vallen)
{
	kumo_tchdb* ctx = reinterpret_cast<kumo_tchdb*>(data);

	kumo_tchdb_update_ctx upctx = { val, vallen };

	return tchdbputproc(ctx->db,
			key, keylen,
			val, vallen,
			kumo_tchdb_update_proc, &upctx);
}

static bool kumo_tchdb_del(void* data,
		const char* key, uint32_t keylen,
		uint64_t clocktime)
{
	kumo_tchdb* ctx = reinterpret_cast<kumo_tchdb*>(data);

	char clockbuf[8];
	kumo_storage_clocktime_to(clocktime, clockbuf);

	kumo_tchdb_update_ctx upctx = { clockbuf, 8 };

	// FIXME push the key to ctx->garbage
	return tchdbputproc(ctx->db,
			key, keylen,
			clockbuf, 8,
			kumo_tchdb_update_proc, &upctx);
}

static uint64_t kumo_tchdb_rnum(void* data)
{
	kumo_tchdb* ctx = reinterpret_cast<kumo_tchdb*>(data);
	return tchdbrnum(ctx->db);
}

static bool kumo_tchdb_backup(void* data, const char* dstpath)
{
	kumo_tchdb* ctx = reinterpret_cast<kumo_tchdb*>(data);
	return tchdbcopy(ctx->db, dstpath);
}

static const char* kumo_tchdb_error(void* data)
{
	kumo_tchdb* ctx = reinterpret_cast<kumo_tchdb*>(data);
	return tchdberrmsg(tchdbecode(ctx->db));
}


typedef struct {
	TCXSTR* key;
	TCXSTR* val;
	kumo_tchdb* ctx;
} kumo_tchdb_iterator;

static int kumo_tchdb_for_each(void* data,
		void* user, int (*func)(void* user, void* iterator_data))
{
	kumo_tchdb* ctx = reinterpret_cast<kumo_tchdb*>(data);

	// only one thread can use iterator
	mp::pthread_scoped_lock itlk(ctx->iterator_mutex);

	if(!tchdbiterinit(ctx->db)) {
		return -1;
	}

	kumo_tchdb_iterator it = { NULL, NULL, NULL };
	it.key = tcxstrnew(); if(!it.key) { return -1; }
	it.val = tcxstrnew(); if(!it.val) { tcxstrdel(it.key); return -1; }
	it.ctx  = ctx;

	while( tchdbiternext3(ctx->db, it.key, it.val) ) {
		if(TCXSTRSIZE(it.val) < 16 || TCXSTRSIZE(it.key) < 8) {
			// FIXME delete it?
			continue;
		}

		int ret = (*func)(user, (void*)&it);
		if(ret < 0) {
			if(it.key != NULL) { tcxstrdel(it.key); }
			if(it.val != NULL) { tcxstrdel(it.val); }
			return ret;
		}

		if(it.key == NULL) {
			it.key = tcxstrnew();
			if(it.key == NULL) {
				if(it.val != NULL) { tcxstrdel(it.val); }
				return -1;
			}
		}

		if(it.val == NULL) {
			it.val = tcxstrnew();
			if(it.val == NULL) {
				tcxstrdel(it.key);
				return -1;
			}
		}
	}

	if(it.key != NULL) { tcxstrdel(it.key); }
	if(it.val != NULL) { tcxstrdel(it.val); }
	return 0;
}

static const char* kumo_tchdb_iterator_key(void* iterator_data)
{
	kumo_tchdb_iterator* it = (kumo_tchdb_iterator*)iterator_data;
	return TCXSTRPTR(it->key);
}

static const char* kumo_tchdb_iterator_val(void* iterator_data)
{
	kumo_tchdb_iterator* it = (kumo_tchdb_iterator*)iterator_data;
	return TCXSTRPTR(it->val);
}

static size_t kumo_tchdb_iterator_keylen(void* iterator_data)
{
	kumo_tchdb_iterator* it = (kumo_tchdb_iterator*)iterator_data;
	return TCXSTRSIZE(it->key);
}

static size_t kumo_tchdb_iterator_vallen(void* iterator_data)
{
	kumo_tchdb_iterator* it = (kumo_tchdb_iterator*)iterator_data;
	return TCXSTRSIZE(it->val);
}


static bool kumo_tchdb_iterator_release_key(void* iterator_data, msgpack_zone* zone)
{
	kumo_tchdb_iterator* it = (kumo_tchdb_iterator*)iterator_data;

	if(!msgpack_zone_push_finalizer(zone, (void (*)(void*))tcxstrdel, it->key)) {
		return false;
	}

	it->key = NULL;
	return true;
}

static bool kumo_tchdb_iterator_release_val(void* iterator_data, msgpack_zone* zone)
{
	kumo_tchdb_iterator* it = (kumo_tchdb_iterator*)iterator_data;

	if(!msgpack_zone_push_finalizer(zone, (void (*)(void*))tcxstrdel, it->val)) {
		return false;
	}

	it->val = NULL;
	return true;
}

static bool kumo_tchdb_iterator_delete(void* iterator_data)
{
	kumo_tchdb_iterator* it = (kumo_tchdb_iterator*)iterator_data;

	const char* key = TCXSTRPTR(it->key);
	size_t keylen = TCXSTRSIZE(it->key);

	return tchdbout(it->ctx->db, key, keylen);
}

static bool kumo_tchdb_iterator_delete_if_older(void* iterator_data, uint64_t if_older)
{
	kumo_tchdb_iterator* it = (kumo_tchdb_iterator*)iterator_data;

	const char* val = TCXSTRPTR(it->val);
	size_t vallen = TCXSTRSIZE(it->val);

	if(vallen < 8 || kumo_clocktime_less(
				kumo_storage_clocktime_of(val),
				if_older)) {

		const char* key = TCXSTRPTR(it->key);
		size_t keylen = TCXSTRSIZE(it->key);

		return tchdbout(it->ctx->db, key, keylen);
	}

	return false;
}


static kumo_storage_op kumo_tchdb_op =
{
	kumo_tchdb_create,
	kumo_tchdb_free,
	kumo_tchdb_open,
	kumo_tchdb_close,
	kumo_tchdb_get,
	kumo_tchdb_set,
	kumo_tchdb_update,
	NULL,
	kumo_tchdb_del,
	kumo_tchdb_rnum,
	kumo_tchdb_backup,
	kumo_tchdb_error,
	kumo_tchdb_for_each,
	kumo_tchdb_iterator_key,
	kumo_tchdb_iterator_val,
	kumo_tchdb_iterator_keylen,
	kumo_tchdb_iterator_vallen,
	kumo_tchdb_iterator_release_key,
	kumo_tchdb_iterator_release_val,
	kumo_tchdb_iterator_delete,
	kumo_tchdb_iterator_delete_if_older,
};

kumo_storage_op kumo_storage_init(void)
{
	return kumo_tchdb_op;
}
