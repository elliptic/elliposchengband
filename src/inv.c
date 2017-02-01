#include "angband.h"

#include <assert.h>

struct _page_s
{
    slot_t start;
    slot_t stop;
};
typedef struct _page_s _page_t, *_page_ptr;

static _page_ptr _page_alloc(slot_t start, slot_t stop)
{
    _page_ptr page = malloc(sizeof(_page_t));
    page->start = start;
    page->stop = stop;
    return page;
}

static void _page_free(_page_ptr page)
{
    if (page) free(page);
}

struct _pagination_s
{
    obj_p   filter;
    int     page_size;
    int     page_count;
    vec_ptr pages;
};
typedef struct _pagination_s _pagination_t, *_pagination_ptr;

static _pagination_ptr _pagination_alloc(obj_p p, int page_size)
{
    _pagination_ptr pagination = malloc(sizeof(_pagination_t));
    pagination->filter = p;
    pagination->page_size = page_size;
    pagination->page_count = 0;
    pagination->pages = vec_alloc((vec_free_f)_page_free);
    return pagination;
}

static void _pagination_free(_pagination_ptr pagination)
{
    if (pagination)
    {
        vec_free(pagination->pages);
        pagination->pages = NULL;
        free(pagination);
    }
}

struct inv_s
{
    cptr    name;
    int     type;
    int     max;
    vec_ptr objects; /* sparse ... grows as needed (up to max+1 if max is set) */
    _pagination_ptr pagination;
};

/* Slots: We are assuming slot <= 26 */
char slot_label(slot_t slot)
{
    assert(slot <= 26);
    if (slot) return slot - 1 + 'a';
    return ' ';
}

slot_t label_slot(char label)
{
    assert('a' <= label && label <= 'z');
    return label - 'a' + 1;
}

/* Helpers */
static void _grow(inv_ptr inv, slot_t slot)
{
    slot_t i;
    assert(slot);
    assert(!inv->max || slot <= inv->max);
    if (slot >= vec_length(inv->objects))
    {
        for (i = vec_length(inv->objects); i <= slot; i++)
            vec_add(inv->objects, NULL);
        assert(slot == vec_length(inv->objects) - 1);
    }
}

/* Creation */
inv_ptr inv_alloc(cptr name, int type, int max)
{
    inv_ptr result = malloc(sizeof(inv_t));
    result->name = name;
    result->type = type;
    result->max = max;
    result->objects = vec_alloc((vec_free_f)obj_free);
    result->pagination = NULL;
    return result;
}

inv_ptr inv_copy(inv_ptr src)
{
    inv_ptr result = malloc(sizeof(inv_t));
    int     i;

    result->name = src->name;
    result->type = src->type;
    result->max = src->max;
    result->objects = vec_alloc((vec_free_f)obj_free);
    result->pagination = NULL;

    for (i = 0; i < vec_length(src->objects); i++)
    {
        obj_ptr obj = vec_get(src->objects, i);
        if (obj)
            vec_add(result->objects, obj_copy(obj));
        else
            vec_add(result->objects, NULL);
    }
    return result;
}

/* Note, a null predicate accepts null objects. This is
 * for INV_EQUIP which is rigidly slot based. Other inventories
 * are lists occupied from [1..N] with valid objects (provided
 * you optimize them). */
static bool _filter(obj_ptr obj, obj_p p)
{
    if (!p) return TRUE;
    if (!obj) return FALSE;
    return p(obj);
}

/* Filtering preserves the slots, but nulls out objects
 * not meeting the predicate. You should never sort a
 * filtered inventory. (cf obj_prompt) */
inv_ptr inv_filter(inv_ptr src, obj_p p)
{
    inv_ptr result = malloc(sizeof(inv_t));
    int     i;

    result->name = src->name;
    result->type = src->type;
    result->max = src->max;
    result->objects = vec_alloc(NULL); /* src owns the objects! */
    result->pagination = NULL;

    for (i = 0; i < vec_length(src->objects); i++)
    {
        obj_ptr obj = vec_get(src->objects, i);

        if (_filter(obj, p))
            vec_add(result->objects, obj);
        else
            vec_add(result->objects, NULL);
    }
    return result;
}

/* floor objects form a linked list. There is no slot structure to
 * preserve. This 'fake inventory' is useful for obj_prompt */
inv_ptr inv_filter_floor(obj_p p)
{
    inv_ptr    result = malloc(sizeof(inv_t));
    cave_type *c_ptr = &cave[py][px];
    int        this_o_idx, next_o_idx = 0;

    result->name = "Floor";
    result->type = INV_FLOOR;
    result->max = 0;
    result->objects = vec_alloc(NULL); /* o_list owns the objects! */
    result->pagination = NULL;

    vec_add(result->objects, NULL); /* slot 0 is invalid */

    for (this_o_idx = c_ptr->o_idx; this_o_idx; this_o_idx = next_o_idx)
    {
        object_type *obj = &o_list[this_o_idx];

        assert(obj);
        next_o_idx = obj->next_o_idx;
        if (_filter(obj, p))
            vec_add(result->objects, obj);
    }
    return result;
}

/* Eventually, shops will be rewritten and this can disappear.
 * For now, it is useful for a couple of obj_prompt scenarios
 * like Inspect. We do not preserve slot structure. */
inv_ptr inv_filter_home(obj_p p)
{
    inv_ptr     result = malloc(sizeof(inv_t));
    store_type *st_ptr = &town[1].store[STORE_HOME];
    int         i;

    result->name = "Home";
    result->type = INV_HOME;
    result->max = 0;
    result->objects = vec_alloc(NULL); /* shop owns the objects! */
    result->pagination = NULL;

    vec_add(result->objects, NULL); /* slot 0 is invalid */

    for (i = 0; i < st_ptr->stock_num; i++)
    {
        obj_ptr obj = &st_ptr->stock[i];
        assert(obj);
        if (!obj->k_idx) continue;
        if (!_filter(obj, p)) continue;
        vec_add(result->objects, obj);
    }
    return result;
}

void inv_free(inv_ptr inv)
{
    if (inv)
    {
        vec_free(inv->objects);
        inv->objects = NULL;
        _pagination_free(inv->pagination);
        inv->pagination = NULL;
        inv->name = NULL;
        free(inv);
    }
}

/* Adding, Removing and Sorting
 * Note: We separate adding and combining in order to support
 * the autopicker, statistics gathering, marking objects as
 * 'touched' and other things that higher level clients need. */
static void _add_aux(inv_ptr inv, obj_ptr obj, slot_t slot)
{
    obj_ptr   copy;
    obj_loc_t loc = {0};
    int       ct = obj->number;

    if (inv->type == INV_EQUIP)
        ct = 1;

    assert(1 <= slot && slot <= inv->max);

    loc.where = inv->type;
    loc.slot = slot;

    copy = obj_copy(obj);
    copy->loc = loc;
    copy->number = ct;
    obj_clear_dun_info(copy);

    if (slot >= vec_length(inv->objects))
        _grow(inv, slot);
    vec_set(inv->objects, slot, copy);

    obj->number -= ct;
}

slot_t inv_add(inv_ptr inv, obj_ptr obj)
{
    slot_t slot;
    for (slot = 1; slot < vec_length(inv->objects); slot++)
    {
        obj_ptr old = vec_get(inv->objects, slot);
        if (!old)
        {
            _add_aux(inv, obj, slot);
            return slot;
        }
    }
    if (!inv->max || slot <= inv->max)
    {
        _add_aux(inv, obj, slot);
        return slot;
    }
    return 0;
}

void inv_add_at(inv_ptr inv, obj_ptr obj, slot_t slot)
{
    _add_aux(inv, obj, slot);
}

/* This version of combine will place all of obj->number
 * into a single slot, combining only if there is room. */
slot_t inv_combine(inv_ptr inv, obj_ptr obj)
{
    slot_t slot;

    assert(obj->number);

    for (slot = 1; slot < vec_length(inv->objects); slot++)
    {
        obj_ptr dest = vec_get(inv->objects, slot);
        if (!dest) continue;
        if ( obj_can_combine(dest, obj, inv->type)
          && dest->number + obj->number <= OBJ_STACK_MAX )
        {
            obj_combine(dest, obj, inv->type);
            return slot;
        }
    }
    assert(obj->number);
    return 0;
}

/* This version will combine the obj pile (if pile it be)
 * into as many slots as possible, splitting the pile in
 * the process. I think stores need this behaviour. We
 * cannot return a single slot, so we return the number
 * of items in the pile that we found room for. */
int inv_combine_ex(inv_ptr inv, obj_ptr obj)
{
    int    ct = 0;
    slot_t slot;

    assert(obj->number);

    /* combine obj with as many existing slots as possible */
    for (slot = 1; slot < vec_length(inv->objects); slot++)
    {
        obj_ptr dest = vec_get(inv->objects, slot);
        if (!dest) continue;
        if (obj_can_combine(dest, obj, inv->type))
        {
            ct += obj_combine(dest, obj, inv->type);
            if (!obj->number) break;
        }
    }
    return ct;
}

bool inv_optimize(inv_ptr inv)
{
    slot_t slot, seek;
    bool result = FALSE;
    for (slot = 1; slot < vec_length(inv->objects); slot++)
    {
        obj_ptr dest = vec_get(inv->objects, slot);
        if (!dest) continue;
        if (!dest->number)
        {
            vec_set(inv->objects, slot, NULL); /* free */
            dest = NULL;
            result = TRUE;
            continue;
        }
        for (seek = slot + 1; seek < vec_length(inv->objects); seek++)
        {
            obj_ptr src = vec_get(inv->objects, seek);
            if (!src) continue;
            if (obj_combine(dest, src, inv->type))
            {
                result = TRUE;
                if (!src->number)
                    vec_set(inv->objects, seek, NULL);
            }
        }
    }
    if (inv_sort(inv)) result = TRUE;
    return result;
}

void inv_remove(inv_ptr inv, slot_t slot)
{
    obj_ptr obj;

    assert(slot);
    obj = inv_obj(inv, slot);
    if (obj)
        vec_set(inv->objects, slot, NULL); /* free */
}

void inv_clear(inv_ptr inv)
{
    vec_clear(inv->objects);
}

bool inv_sort(inv_ptr inv)
{
    int start = 1, stop = vec_length(inv->objects) - 1;
    if (start == stop) return FALSE;
    inv_for_each(inv, obj_clear_scratch);
    if (!vec_is_sorted_range(inv->objects, start, stop, (vec_cmp_f)obj_cmp))
    {
        slot_t slot;
        vec_sort_range(inv->objects, start, stop, (vec_cmp_f)obj_cmp);
        for (slot = start; slot <= stop; slot++)
        {
            obj_ptr obj = vec_get(inv->objects, slot);
            if (obj)
            {
                obj->loc.slot = slot;
                assert(obj->loc.where == inv->type);
            }
        }
        return TRUE; /* So clients can notify the player ... */
    }
    return FALSE;
}

void inv_swap(inv_ptr inv, slot_t left, slot_t right)
{
    obj_ptr obj;

    _grow(inv, MAX(left, right)); /* force allocation of slots */
    vec_swap(inv->objects, left, right);

    obj = vec_get(inv->objects, left);
    if (obj) obj->loc.slot = left;
    obj = vec_get(inv->objects, right);
    if (obj) obj->loc.slot = right;
}

/* Iterating, Searching and Accessing Objects (Predicates are always optional) */
obj_ptr inv_obj(inv_ptr inv, slot_t slot)
{
    assert(slot);
    if (slot >= vec_length(inv->objects))
    {
        assert(!inv->max || slot <= inv->max);
        return NULL;
    }
    return vec_get(inv->objects, slot);
}

slot_t inv_first(inv_ptr inv, obj_p p)
{
    return inv_next(inv, p, 0);
}

slot_t inv_next(inv_ptr inv, obj_p p, slot_t prev_match)
{
    int slot;
    for (slot = prev_match + 1; slot < vec_length(inv->objects); slot++)
    {
        obj_ptr obj = inv_obj(inv, slot);
        if (_filter(obj, p)) return slot;
    }
    return 0;
}

slot_t inv_last(inv_ptr inv, obj_p p)
{
    int slot;
    for (slot = vec_length(inv->objects) - 1; slot > 0; slot--)
    {
        obj_ptr obj = inv_obj(inv, slot);
        if (_filter(obj, p)) return slot;
    }
    return 0;
}

slot_t inv_find_art(inv_ptr inv, int which)
{
    int slot;
    for (slot = 1; slot < vec_length(inv->objects); slot++)
    {
        obj_ptr obj = inv_obj(inv, slot);
        if (obj && obj->name1 == which) return slot;
    }
    return 0;
}

slot_t inv_find_ego(inv_ptr inv, int which)
{
    int slot;
    for (slot = 1; slot < vec_length(inv->objects); slot++)
    {
        obj_ptr obj = inv_obj(inv, slot);
        if (obj && obj->name2 == which) return slot;
    }
    return 0;
}

slot_t inv_find_obj(inv_ptr inv, int tval, int sval)
{
    int slot;
    for (slot = 1; slot < vec_length(inv->objects); slot++)
    {
        obj_ptr obj = inv_obj(inv, slot);
        if (!obj) continue;
        if (obj->tval != tval) continue;
        if (sval != SV_ANY && obj->sval != sval) continue;
        return slot;
    }
    return 0;
}

void inv_for_each(inv_ptr inv, obj_f f)
{
    int slot;
    assert(f);
    for (slot = 1; slot < vec_length(inv->objects); slot++)
    {
        obj_ptr obj = inv_obj(inv, slot);
        if (obj)
            f(obj);
    }
}

void inv_for_each_that(inv_ptr inv, obj_f f, obj_p p)
{
    int slot;
    assert(f);
    assert(p);
    for (slot = 1; slot < vec_length(inv->objects); slot++)
    {
        obj_ptr obj = inv_obj(inv, slot);
        if (_filter(obj, p))
            f(obj);
    }
}

void inv_for_each_slot(inv_ptr inv, slot_f f)
{
    int slot;
    int max = inv->max ? inv->max : vec_length(inv->objects) - 1;
    assert(f);
    for (slot = 1; slot <= max; slot++)
        f(slot);
}

slot_t inv_random_slot(inv_ptr inv, obj_p p)
{
    int ct = 0;
    int slot;

    for (slot = 1; slot < vec_length(inv->objects); slot++)
    {
        obj_ptr obj = inv_obj(inv, slot);
        if (_filter(obj, p))
            ct++;
    }

    if (ct)
    {
        int which = randint0(ct);
        for (slot = 1; slot < vec_length(inv->objects); slot++)
        {
            obj_ptr obj = inv_obj(inv, slot);
            if (_filter(obj, p))
            {
                if (!which) return slot;
                which--;
            }
        }
    }
    return 0;
}

/* Properties of the Entire Inventory */
int inv_weight(inv_ptr inv, obj_p p)
{
    int wgt = 0;
    int slot;
    for (slot = 1; slot < vec_length(inv->objects); slot++)
    {
        obj_ptr obj = inv_obj(inv, slot);
        if (obj && _filter(obj, p))
            wgt += obj->weight * obj->number;
    }
    return wgt;
}

int inv_count(inv_ptr inv, obj_p p)
{
    int ct = 0;
    int slot;
    for (slot = 1; slot < vec_length(inv->objects); slot++)
    {
        obj_ptr obj = inv_obj(inv, slot);
        if (obj && _filter(obj, p))
            ct += obj->number;
    }
    return ct;
}

int inv_count_slots(inv_ptr inv, obj_p p)
{
    int ct = 0;
    int slot;
    for (slot = 1; slot < vec_length(inv->objects); slot++)
    {
        obj_ptr obj = inv_obj(inv, slot);
        if (_filter(obj, p))
            ct++;
    }
    return ct;
}

int inv_loc(inv_ptr inv)
{
    return inv->type;
}

int inv_max(inv_ptr inv)
{
    return inv->max;
}

cptr inv_name(inv_ptr inv)
{
    return inv->name;
}

/* Menus and Display */
void inv_display(inv_ptr inv, slot_t start, slot_t stop, obj_p p, doc_ptr doc, slot_display_f slot_f, int flags)
{
    slot_t slot;
    int    xtra = 0;

    if (!stop)
        stop = vec_length(inv->objects) - 1;

    if (flags & INV_SHOW_FAIL_RATES)
        xtra = 6;  /* " 98.7%" */
    else if (show_weights)
        xtra = 9;  /* " 123.0 lbs" */

    inv_calculate_labels(inv, start, stop);

    doc_insert(doc, "<style:table>");
    for (slot = start; slot <= stop; slot++)
    {
        obj_ptr obj = inv_obj(inv, slot);

        if (!_filter(obj, p)) continue;
        if (!obj)
        {
            doc_printf(doc, " %c) ", inv_slot_label(inv, slot));
            if (show_item_graph)
                doc_insert(doc, "  ");
            if (slot_f)
                slot_f(doc, slot);
            doc_insert(doc, "<color:D>Empty</color>\n");
        }
        else
        {
            char name[MAX_NLEN];
            doc_style_t style = *doc_current_style(doc);
            object_desc(name, obj, OD_COLOR_CODED);
            doc_printf(doc, " %c) ", inv_slot_label(inv, slot));
            if (show_item_graph)
            {
                doc_insert_char(doc, object_attr(obj), object_char(obj));
                doc_insert(doc, " ");
            }
            if (slot_f)
                slot_f(doc, slot);
            if (xtra)
            {
                style.right = doc_width(doc) - xtra;
                doc_push_style(doc, &style);
            }
            doc_printf(doc, "%s", name);
            if (xtra)
                doc_pop_style(doc);
            if (flags & INV_SHOW_FAIL_RATES)
            {
                if (object_is_aware(obj) && obj_is_identified_fully(obj))
                {
                    int fail = device_calc_fail_rate(obj);
                    if (fail == 1000)
                        doc_printf(doc, "<tab:%d> %3d%%", doc_width(doc) - xtra, fail/10);
                    else
                        doc_printf(doc, "<tab:%d> %2d.%d%%", doc_width(doc) - xtra, fail/10, fail%10);
                }
            }
            else if (show_weights)
            {
                int wgt = obj->weight * obj->number;
                doc_printf(doc, "<tab:%d> %3d.%d lbs", doc_width(doc) - xtra, wgt/10, wgt%10);
            }
            doc_newline(doc);
        }
    }
    doc_insert(doc, "</style>");
}

char inv_slot_label(inv_ptr inv, slot_t slot)
{
    obj_ptr obj = inv_obj(inv, slot);
    if (obj && obj->scratch)
        return obj->scratch;
    return ' ';
}

slot_t inv_label_slot(inv_ptr inv, char label)
{
    slot_t slot;
    for (slot = 1; slot < vec_length(inv->objects); slot++)
    {
        obj_ptr obj = vec_get(inv->objects, slot);
        if (!obj) continue;
        if (obj->scratch != label) continue;
        return slot;
    }
    return 0;
}

void inv_calculate_labels(inv_ptr inv, slot_t start, slot_t stop)
{
    slot_t slot;
    if (!stop)
        stop = vec_length(inv->objects) - 1;
    inv_for_each(inv, obj_clear_scratch);
    /* Initialize by ordinal */
    for (slot = start; slot <= stop; slot++)
    {
        obj_ptr obj = inv_obj(inv, slot);
        if (obj)
            obj->scratch = slot_label(slot - start + 1);
    }

    /* Inscription overrides don't function is shops */
    if (inv->type == INV_STORE || inv->type == INV_HOME)
        return;

    /* Override by inscription (e.g. @mf) */
    for (slot = start; slot <= stop; slot++)
    {
        obj_ptr obj = inv_obj(inv, slot);
        if (obj)
        {
            char label = obj_label(obj);
            if (label)
            {
                /* override this label if in use ... */
                slot_t slot2 = inv_label_slot(inv, label);
                if (slot2)
                    inv_obj(inv, slot2)->scratch = ' ';
                /* ... before marking this object */
                obj->scratch = label;
            }
        }
    }
}

/* Pagination */
void inv_paginate(inv_ptr inv, obj_p p, int page_size)
{
    slot_t start, pos, max;

    assert(!inv->pagination); /* you forgot to call inv_unpaginate() */
    inv->pagination = _pagination_alloc(p, page_size);

    max = inv_last(inv, p);
    if (!max) return;

    start = inv_first(inv, p);
    assert(start);

    for (pos = start; pos <= max; pos++)
    {
        _page_ptr page;
        int       ct = 1;

        while (ct < page_size)
        {
            assert(pos && pos <= max);
            pos = inv_next(inv, p, pos);
            ct++;
            if (!pos) break;
        }
        if (!pos) pos = max;
        page = _page_alloc(start, pos);
        vec_add(inv->pagination->pages, page);
        inv->pagination->page_count++;

        start = pos + 1;
    }
}

int inv_page_count(inv_ptr inv)
{
    assert(inv->pagination);
    return inv->pagination->page_count;
}

void inv_display_page(inv_ptr inv, int page, doc_ptr doc, int flags)
{
    _page_ptr page_ptr;
    assert(inv->pagination);
    assert(0 <= page && page < vec_length(inv->pagination->pages));
    page_ptr = vec_get(inv->pagination->pages, page);
    inv_display(
        inv, 
        page_ptr->start, page_ptr->stop,
        inv->pagination->filter,
        doc, NULL, flags
    );
}

void inv_unpaginate(inv_ptr inv)
{
    assert(inv->pagination);
    _pagination_free(inv->pagination);
    inv->pagination = NULL;
}

/* Savefiles */
void inv_load(inv_ptr inv, savefile_ptr file)
{
    int i, ct, slot;
    vec_clear(inv->objects);
    ct = savefile_read_s32b(file);
    for (i = 0; i < ct; i++)
    {
        obj_ptr obj = malloc(sizeof(object_type));
        object_wipe(obj);

        slot = savefile_read_s32b(file);
        obj_load(obj, file);

        if (slot >= vec_length(inv->objects))
            _grow(inv, slot);
        vec_set(inv->objects, slot, obj);
    }
}

void inv_save(inv_ptr inv, savefile_ptr file)
{
    int ct = inv_count_slots(inv, obj_exists);
    int slot;

    savefile_write_s32b(file, ct);
    for (slot = 1; slot < vec_length(inv->objects); slot++)
    {
        obj_ptr obj = inv_obj(inv, slot);
        if (obj)
        {
            savefile_write_s32b(file, slot);
            obj_save(obj, file);
            ct--;
        }
    }
    assert(ct == 0);
}
