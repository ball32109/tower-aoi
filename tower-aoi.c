#include <lua.h>
#include <lauxlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define OBJECTPOOL 		64
#define DEFAULT_LEVEL 	8
#define COMMON_LEVEL 	0

#define MARKER 			0x01
#define WATCHER 		0x02
#define WATCHERMAKKER 	0x03

struct object;

struct slot {
	int id;
	struct object *obj;
	int next;
};

struct table {
	int size;
	int lastfree;
	struct slot *slot;
};

struct point {
	float x;
	float y;
};

struct object {
	struct point cur;
	int id;
	int level;
	int range;
	int type;
	struct object *next;
};

struct objectpool_list {
	struct objectpool_list *next;
	struct object pool[OBJECTPOOL];
};

struct objectpool {
	struct objectpool_list *pool;
	struct object *freelist;
};

struct tile {
	int size;
	int x;
	int y;
	struct table *markers;
	struct table *watchers;
};

struct map {
	int realwidth;
	int realhigh;
	int width;
	int high;
	int max_x_index;
	int max_y_index;
	int tile_len;
	int tile_sz;
	struct tile *tiles;
};

struct aoi_context {
	struct map map;
	struct objectpool *pool;
};

void 
table_insert(struct table *t,int id,struct object *obj);

void 
rehash(struct table *t) {
	struct slot *oslot = t->slot;
	int osize = t->size;
	t->size = 2 * osize;
	t->lastfree = t->size - 1;
	t->slot = malloc(t->size * sizeof(struct slot));
	
	int i;
	for (i = 0; i < t->size; i++) {
		struct slot *s = &t->slot[i];
		s->id = -1;
		s->obj = NULL;
		s->next = -1;
	}
	
	for (i = 0; i < osize; i++) {
		struct slot * s = &oslot[i];
		if (s->obj)
			table_insert(t, s->id, s->obj);
	}
	free(oslot);
}

struct table*
table_create() {
	struct table *t = malloc(sizeof(*t));
	t->size = 64;
	t->lastfree = t->size - 1;
	t->slot = malloc(t->size * sizeof(struct slot));

	int i = 0;
	for(;i < t->size;++i) {
		struct slot *s = &t->slot[i];
		s->id = -1;
		s->obj = 0;
		s->next = -1;
	}
	return t;
}

void
table_release(struct table *t) {
	free(t->slot);
	free(t);
}

struct slot*
mainposition(struct table *t,int id) {
	int hash = id & (t->size - 1);
	return &t->slot[hash];
}

void 
table_insert(struct table *t,int id,struct object *obj) {
	struct slot *s = mainposition(t,id);
	if (s->obj == NULL) {
		s->id = id;
		s->obj = obj;
		return;
	}

	if (mainposition(t,s->id) != s) {
		struct slot *last = mainposition(t,s->id);
		while (last->next != s-t->slot) {
			last = &t->slot[last->next];
		}
		int temp_id = s->id;
		struct object *temp_obj = s->obj;

		last->next = s->next;
		s->id = id;
		s->obj = obj;
		s->next = -1;
		if (temp_obj)
			table_insert(t,temp_id,temp_obj);
		return;
	}

	while(t->lastfree >= 0) {
		struct slot *tmp = &t->slot[t->lastfree--];
		if (tmp->id == -1 && tmp->next != -1) {
			tmp->id = id;
			tmp->obj = obj;
			tmp->next = s->next;
			s->next = (int)(tmp-t->slot);
			return;
		}
	}
	rehash(t);
}

struct object*
table_delete(struct table *t,int id) {
	int hash = id & (t->size-1);
	struct slot *s = &t->slot[hash];
	struct slot *next = s;
	for(;;) {
		if (next->id == id) {
			struct object *obj = next->obj;
			next->obj = NULL;
			if (next != s) {
				s->next = next->next;
				next->next = -1;
				if (t->lastfree < next - t->slot) 
					t->lastfree = next - t->slot;
			}
			else {
				if (next->next != -1) {
					struct slot *nextslot = &t->slot[next->next];
					next->id = nextslot->id;
					next->obj = nextslot->obj;
					next->next = nextslot->next;
					nextslot->id = 0;
					nextslot->obj = 0;
					nextslot->next = -1;
				}
			}
			return obj;
		}
		if (next->next < 0)
			return NULL;

		s = next;
		next = &t->slot[next->next];
	}
	return NULL;
}

struct object*
table_find(struct table *t,int id) {
	struct slot *s = mainposition(t,id);
	for(;;) {
		if (s->id == id) 
			return s->obj;
		
		if (s->next == -1)
			break;
		s = &t->slot[s->next];
	}
	return NULL;
}

struct tile *tile_withrc(struct map *m,int r,int c);

void
tile_init(struct map *m) {
	m->tiles = malloc(m->tile_sz * sizeof(struct tile));
	int x, y;
	for (x = 0; x <= m->max_x_index; x++) {
		for (y = 0; y <= m->max_y_index; y++) {
			struct tile* tl = tile_withrc(m, y, x);
			tl->x = x;
			tl->y = y;
			tl->markers = table_create();
			tl->watchers = table_create();
		}
	}
}

struct tile*
tile_withrc(struct map *m,int r,int c) {
	if (c > m->max_x_index || r > m->max_y_index)
		return NULL;
	return &m->tiles[r * (m->max_x_index + 1) + c];
}

struct tile*
tile_withpos(struct map *m,struct point *pos) {
	int x = pos->x / m->tile_len;
	int y = pos->y / m->tile_len;
	if (x > m->max_x_index || y > m->max_y_index)
		return NULL;
	return tile_withrc(m,y,x);
}

int 
calc_rect(struct map *m, struct point *pos, int range, struct point *bl, struct point *tr) {
	struct tile *tl = tile_withpos(m, pos);
	if (tl == NULL)	
		return -1;

	bl->x = tl->x - range;
	bl->y = tl->y - range;
	tr->x = tl->x + range;
	tr->y = tl->y + range;

	if (bl->x < 0)
		bl->x = 0;
	if (bl->y < 0)
		bl->y = 0;
	if (tr->x > m->max_x_index)
		tr->x = m->max_x_index;
	if (tr->y > m->max_y_index)
		tr->y = m->max_y_index;

	return 0;
}

void 
make_table(lua_State *L,struct table *t,int stack) {
	int index = 1;
	int i;
	for(i = 0;i < t->size;i++) {
		if (t->slot[i].obj) {
			lua_pushinteger(L, t->slot[i].obj->id);
			lua_rawseti(L, stack - 1, index++);
		}
	}
}

int
add_marker(lua_State *L,struct map *m,struct object *obj) {
	struct tile *tl = tile_withpos(m,&obj->cur);
	if (tl == NULL) 
		return -1;
	table_insert(tl->markers,obj->id,obj);
	make_table(L,tl->watchers,-1);
	return 0;
}

int 
remove_marker(lua_State *L,struct map *m,struct object *obj) {
	struct tile *tl = tile_withpos(m,&obj->cur);
	if (tl == NULL) 
		return -1;
	table_delete(tl->markers,obj->id);
	make_table(L,tl->watchers,-1);
	return 0;
}

int
add_watcher(lua_State *L,struct map *m,struct object *obj) {
	struct point bl,tr;
	if (calc_rect(m,&obj->cur,obj->range,&bl,&tr) < 0)
		return -1;
	
	int x,y;
	for(y = bl.y;y <= tr.y;y++) {
		for(x = bl.x;x <= tr.x;x++) {
			struct tile *tl = tile_withrc(m,y,x);
			if (tl == NULL)
				return -1;
			table_insert(tl->watchers,obj->id,obj);
			make_table(L,tl->markers,-1);
		}
	}
	return 0;
}

int
remove_watcher(lua_State *L,struct map *m,struct object *obj) {
	struct point bl,tr;
	if (calc_rect(m,&obj->cur,obj->range,&bl,&tr) < 0)
		return -1;
	
	int x,y;
	for(y = bl.y;y <= tr.y;y++) {
		for(x = bl.x;x <= tr.x;x++) {
			struct tile *tl = tile_withrc(m,y,x);
			if (tl == NULL)
				return -1;
			table_delete(tl->watchers,obj->id);
		}
	}
	return 0;
}

int
update_marker(lua_State *L,struct map *m,struct object *obj,struct point *np) {
	struct tile *otl = tile_withpos(m,&obj->cur);
	if (otl == NULL)
		return -1;

	obj->cur.x = np->x;
	obj->cur.y = np->y;
	struct tile *ntl = tile_withpos(m,&obj->cur);
	if (ntl == NULL)
		return -1;

	if (otl == ntl)
		return 0;

	table_delete(otl->markers,obj->id);
	make_table(L,otl->watchers,-1);
	table_insert(ntl->markers,obj->id,obj);
	make_table(L,ntl->watchers,-2);

	return 0;
}

int 
update_watcher(lua_State *L,struct map *m,struct object *obj,struct point *np) {
	struct point op = obj->cur;
	struct tile *otl = tile_withpos(m,&obj->cur);
	if (otl == NULL)
		return -1;

	obj->cur.x = np->x;
	obj->cur.y = np->y;
	struct tile *ntl = tile_withpos(m,&obj->cur);
	if (ntl == NULL)
		return -1;

	if (otl == ntl)
		return 0;


	struct point obl, otr;
	if (calc_rect(m, &op, obj->range, &obl, &otr) < 0)
		return -1;

	struct point nbl, ntr;
	if (calc_rect(m, &obj->cur, obj->range, &nbl, &ntr) < 0)
		return -1;

	int x, y;
	for (y = nbl.y; y <= ntr.y; y++) {
		for (x = nbl.x; x <= ntr.x; x++) {
			if (x >= obl.x && x <= otr.x && y >= obl.y && y <= otr.y)
				continue;

			struct tile *tl = tile_withrc(m,y,x);
			if (tl == NULL)
				return -1;

			table_delete(tl->watchers,obj->id);
			make_table(L,tl->markers,-1);
		}
	}

	for (y = obl.y; y <= otr.y; y++) {
		for (x = obl.x; x <= otr.x; x++) {
			if (x >= nbl.x && x <= ntr.x && y >= nbl.y && y <= ntr.y)
				continue;

			struct tile *tl = tile_withrc(m,y,x);
			if (tl == NULL)
				return -1;

			table_insert(tl->watchers,obj->id,obj);
			make_table(L,tl->markers,-2);
		}
	}
	return 0;

}

int 
add_object(lua_State *L,struct map *m,struct object *obj) {
	if (obj->type & MARKER) {
		lua_newtable(L);
		add_marker(L,m,obj);
	} 
	if (obj->type & WATCHER) {
		lua_newtable(L);
		add_watcher(L,m,obj);
	}
	return 0;
}

int
update_object(lua_State *L,struct map *m,struct object *obj,struct point *np) {
	if (obj->type & MARKER) {
		lua_newtable(L);
		lua_newtable(L);
		update_marker(L,m,obj,np);
	}
	if (obj->type & WATCHER) {
		lua_newtable(L);
		lua_newtable(L);
		update_watcher(L,m,obj,np);
	}
	return 0;
}

int
remove_object(lua_State *L,struct map *m,struct object *obj) {
	if (obj->type & MARKER) {
		lua_newtable(L);
		remove_marker(L,m,obj);
	} 
	if (obj->type & WATCHER) {
		remove_watcher(L,m,obj);
	}
	return 0;
}

int
_aoi_new(lua_State *L) {
	int realwidth = luaL_checkinteger(L, 1);
	int realhigh = luaL_checkinteger(L, 2);
	int tile_len = luaL_checkinteger(L, 3);

	int max_x_index = realwidth / tile_len - 1;
	int max_y_index = realhigh / tile_len - 1;

	int width = (max_x_index + 1) * tile_len;
	int high = (max_y_index + 1) * tile_len;

	struct aoi_context *aoictx = malloc(sizeof(*aoictx));
	memset(aoictx,0,sizeof(*aoictx));

	aoictx->pool = malloc(sizeof(*aoictx->pool));
	memset(aoictx->pool,0,sizeof(*aoictx->pool));

	aoictx->map.realwidth = realwidth;
	aoictx->map.realhigh = realhigh;
	aoictx->map.width = width;
	aoictx->map.high = high;
	aoictx->map.max_x_index = max_x_index;
	aoictx->map.max_y_index = max_y_index;
	aoictx->map.tile_len = tile_len;   //tile length
	aoictx->map.tile_sz = (max_x_index + 1) * (max_y_index + 1);   //amount of tiles in map
	
	tile_init(&aoictx->map);

	lua_pushlightuserdata(L, aoictx);
	return 1;
}

int 
_aoi_delete(lua_State *L) {
	struct aoi_context *aoi = lua_touserdata(L, 1);
	struct objectpool_list *p = aoi->pool->pool;
	while(p) {
		struct objectpool_list *tmp = p;
		p = p->next;
		free(tmp);
	}
	int i;
	for(i = 0;i < aoi->map.tile_sz;i++) {
		table_release(aoi->map.tiles[i].markers);
		table_release(aoi->map.tiles[i].watchers);
	}
	free(aoi->map.tiles);
	free(aoi);
	return 0;
}

int
_aoi_enter(lua_State *L) {
	struct aoi_context *aoi = lua_touserdata(L, 1);
	int id = luaL_checkinteger(L, 2);
	float curx = luaL_checknumber(L, 3);
	float cury = luaL_checknumber(L, 4);
	int level = luaL_checkinteger(L,5);
	int range = luaL_checkinteger(L,6);

	if (curx < 0 || cury < 0 || curx >= aoi->map.width || cury >= aoi->map.high) {
		luaL_error(L,"[_aoi_enter]invalid cur pos[%d:%d]",curx,cury);
		return 0;
	}

	struct object *obj;
	if (aoi->pool->freelist) {
		obj = aoi->pool->freelist;
		aoi->pool->freelist = obj->next;
	} else {
		struct objectpool_list *opl = malloc(sizeof(*opl));
		struct object * temp = opl->pool;
		memset(temp,0,sizeof(struct object) * OBJECTPOOL);
		int i;
		for (i=1;i<OBJECTPOOL;i++) {
			temp[i].next = &temp[i+1];
		}
		temp[OBJECTPOOL-1].next = NULL;
		opl->next = aoi->pool->pool;
		aoi->pool->pool = opl;
		obj = &temp[0];
		aoi->pool->freelist = &temp[1];
	}
	
	obj->id = id;
	obj->type = WATCHERMAKKER;
	obj->cur.x = curx;
	obj->cur.y = cury;
	obj->level = level;
	obj->range = range;

	lua_pushlightuserdata(L, obj);

	add_object(L,&aoi->map,obj);
	
	return 3;
}

int 
_aoi_leave(lua_State *L) {
	struct aoi_context *aoi = lua_touserdata(L, 1);
	struct object *obj = lua_touserdata(L, 2);

	remove_object(L,&aoi->map,obj);

	obj->next = aoi->pool->freelist;
	aoi->pool->freelist = obj;

	return 1;
}

int 
_aoi_update(lua_State *L) {
	struct aoi_context *aoi = lua_touserdata(L, 1);
	struct object *obj = lua_touserdata(L, 2);

	struct point np;
	np.x = luaL_checknumber(L, 3);
	np.y = luaL_checknumber(L, 4);

	if (np.x >= aoi->map.width || np.y >= aoi->map.high) {
		luaL_error(L,"[_aoi_update]invalid pos[%d:%d].",np.x,np.y);
		return 0; 
	}

	if (np.x < 0 || np.y < 0) {
		luaL_error(L,"[_aoi_update]invalid pos[%d:%d].",np.x,np.y);
		return 0; 
	}

	update_object(L,&aoi->map,obj,&np);
	return 4;
}

int
_aoi_viewlist(lua_State *L) {

}

int 
luaopen_toweraoi_c(lua_State *L)
{
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "aoi_new", _aoi_new},
		{ "aoi_delete", _aoi_delete},
		{ "aoi_enter", _aoi_enter},
		{ "aoi_leave", _aoi_leave},
		{ "aoi_update", _aoi_update},
		{ "aoi_viewlist", _aoi_viewlist},
		{ NULL, NULL },
	};

	lua_createtable(L, 0, (sizeof(l)) / sizeof(luaL_Reg) - 1);
	luaL_setfuncs(L, l, 0);
	return 1;
}

