#include "config.h"

#include <stdio.h>
#include <stdlib.h>

#include "cache/cache.h"

#include "vtim.h"
#include "vcc_prometheus_if.h"

#include "vapi/vsm.h"
#include "vapi/vsc.h"
#include "vsb.h"

#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdint.h>

struct prometheus_priv
{
	unsigned magic;
#define PROMETHEUS_PRIV_OBJECT_MAGIC 0x13391339
	VTAILQ_HEAD(, prometheus_group)
	groups;
	struct vsb *vsb;
};

struct prometheus_group
{
	unsigned magic;
#define PROMETHEUS_GROUP_OBJECT_MAGIC 0x13391339
	VTAILQ_ENTRY(prometheus_group)
	list;
	VTAILQ_HEAD(, prometheus_value)
	v;
	char *group_name;
	char *description;
};

struct prometheus_value
{
	unsigned magic;
#define PROMETHEUS_VALUE_OBJECT_MAGIC 0x13391339
	VTAILQ_ENTRY(prometheus_value)
	list;
	double val;
	char *id;
	char *server;
	char *backend;
	char *target;
	char *type;
};

static void
vmod_priv_free(VRT_CTX, void *p)
{
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	free(p);
}

const struct Groupings
{
	const char *prefix;
	const char *desc;
} groupings[] = {
	{"varnish_main_fetch", "Number of fetches"},
	{"varnish_main_sessions", "Number of sessions"},
	{"varnish_main_worker_threads", "Number of worker threads"},
	{NULL, NULL},
};

const char *target_indentifers[] = {
	"varnish_lck_collisions",
	"varnish_lck_created",
	"varnish_lck_destroyed",
	"varnish_lck_operations",
	NULL,
};

const char *type_indentifers[] = {
	"varnish_sma_c_bytes",
	"varnish_sma_c_fail",
	"varnish_sma_c_freed",
	"varnish_sma_c_req",
	"varnish_sma_g_alloc",
	"varnish_sma_g_bytes",
	"varnish_sma_g_space",
	"varnish_smf_c_bytes",
	"varnish_smf_c_fail",
	"varnish_smf_c_freed",
	"varnish_smf_c_req",
	"varnish_smf_g_alloc",
	"varnish_smf_g_bytes",
	"varnish_smf_g_smf_frag",
	"varnish_smf_g_smf_large",
	"varnish_smf_g_smf",
	"varnish_smf_g_space",
	NULL,
};

static const struct vmod_priv_methods priv_vcl_method[1] = {{.magic = VMOD_PRIV_METHODS_MAGIC,
															 .type = "vmod_prometheus_priv_vcl",
															 .fini = vmod_priv_free}};

static char *cleaup_backend_name(char *backend)
{
	if (strncmp(backend, "boot.", 5) == 0 || strncmp(backend, "root:", 5) == 0)
		return backend + 5;

	return backend;
}

static void synth_response(struct prometheus_priv *p)
{
	// This function will empty the queues and render it
	struct prometheus_group *k_item;
	struct prometheus_value *v_item;

	CHECK_OBJ_NOTNULL(p, PROMETHEUS_PRIV_OBJECT_MAGIC);

	while (!VTAILQ_EMPTY(&p->groups))
	{
		k_item = VTAILQ_FIRST(&p->groups);
		CHECK_OBJ_NOTNULL(k_item, PROMETHEUS_GROUP_OBJECT_MAGIC);

		VSB_printf(p->vsb, "# HELP %s %s\n", k_item->group_name, k_item->description);
		VSB_printf(p->vsb, "# TYPE %s gauge\n", k_item->group_name);
		while (!VTAILQ_EMPTY(&k_item->v))
		{
			v_item = VTAILQ_FIRST(&k_item->v);
			CHECK_OBJ_NOTNULL(v_item, PROMETHEUS_VALUE_OBJECT_MAGIC);

			VSB_printf(p->vsb, "%s", k_item->group_name);

			if (v_item->server != NULL && v_item->server != NULL)
			{
				VSB_printf(p->vsb, "{");
				VSB_printf(p->vsb, "server=\"%s\"", v_item->server);
				VSB_printf(p->vsb, ", backend=\"%s\"", cleaup_backend_name(v_item->backend));
				VSB_printf(p->vsb, "}");
			}
			else if (v_item->id != NULL || v_item->target != NULL || v_item->type != NULL)
			{
				VSB_printf(p->vsb, "{");
				if (v_item->id != NULL)
					VSB_printf(p->vsb, "id=\"%s\"", v_item->id);

				if (v_item->target != NULL)
					VSB_printf(p->vsb, "target=\"%s\"", v_item->target);

				if (v_item->type != NULL)
					VSB_printf(p->vsb, "type=\"%s\"", v_item->type);

				VSB_printf(p->vsb, "}");
			}
			VSB_printf(p->vsb, " %g\n", v_item->val);

			VTAILQ_REMOVE(&k_item->v, v_item, list);
			free(v_item->server);
			free(v_item->backend);
			free(v_item->id);
			free(v_item->target);
			free(v_item->type);
			FREE_OBJ(v_item);
		}

		VTAILQ_REMOVE(&p->groups, k_item, list);
		free(k_item->group_name);
		free(k_item->description);
		FREE_OBJ(k_item);
	}
}

static void group_insert(struct prometheus_priv *p, char *group_name, const char *description, struct prometheus_value *v)
{
	struct prometheus_group *iter_group = NULL;
	struct prometheus_group *group = NULL;

	CHECK_OBJ_NOTNULL(v, PROMETHEUS_VALUE_OBJECT_MAGIC);

	VTAILQ_FOREACH(iter_group, &p->groups, list)
	{
		if (strcmp(iter_group->group_name, group_name) == 0 && strcmp(iter_group->description, description) == 0)
		{
			group = iter_group;
			break;
		}
	}

	if (group == NULL)
	{
		ALLOC_OBJ(group, PROMETHEUS_GROUP_OBJECT_MAGIC);
		group->description = strdup(description);
		group->group_name = strdup(group_name);

		VTAILQ_INIT(&group->v);
		VTAILQ_INSERT_TAIL(&p->groups, group, list);
	}

	VTAILQ_INSERT_TAIL(&group->v, v, list);
}

static char *strncat_lower(char *dest, const char *src, size_t n)
{
	size_t dest_len = strlen(dest);
	size_t i;

	for (i = 0; i < n && src[i] != '\0'; i++)
		dest[dest_len + i] = tolower(src[i]);
	dest[dest_len + i] = '\0';

	return dest;
}

static int v_matchproto_(VSC_Iter_f) do_once_cb_first(void *priv, const struct VSC_point *const pt)
{
	//Do something about this probably

	uint64_t val;

	if (pt == NULL)
		return (0);

	AZ(strcmp(pt->ctype, "uint64_t"));
	if (strcmp(pt->name, "MAIN.uptime"))
		return (0);
	val = VSC_Value(pt);
	(void)val;
	return (1);
}

static int v_matchproto_(VSC_iter_f)
	do_once_cb(void *priv, const struct VSC_point *const pt)
{
	// It tries to copy prometheus.go in some way.

	const char *p;

	const char **identifier = NULL;
	struct prometheus_value *v;

	const char *firstdot = NULL;
	const char *lastdot = NULL;
	const char *seconddot = NULL;

	const char *startparentheses = NULL;
	const char *closeparentheses = NULL;

	const char *lastunderscore = NULL;

	struct prometheus_priv *pp = priv;

	const struct Groupings *g;

	//Hold temporary name, this might be superunsafe
	char tmp[200] = {0};

	if (pt == NULL)
		return 0;

	AZ(strcmp(pt->ctype, "uint64_t"));

	for (p = pt->name; *p != '\0'; p++)
	{
		if (*p == '.')
		{
			if (firstdot != NULL && seconddot == NULL)
				seconddot = p;

			if (firstdot == NULL)
				firstdot = p;

			lastdot = p;
		}

		if (*p == '(' && startparentheses == NULL)
			startparentheses = p;

		if (*p == ')')
			closeparentheses = p;

		if (*p == '_' && lastunderscore == NULL)
			lastunderscore = p;
	}

	ALLOC_OBJ(v, PROMETHEUS_VALUE_OBJECT_MAGIC);
	v->val = (double)VSC_Value(pt);

	if(lastdot == NULL || firstdot == NULL)
		return 0;

	// Parse VBE, try to figure out whats backend and server
	if (pt->name[0] == 'V' && pt->name[1] == 'B' && pt->name[2] == 'E')
	{
		strcat(tmp, "varnish_backend_");
		strcat(tmp, lastdot + 1);

		if (startparentheses == NULL && seconddot > firstdot)
		{
			if (seconddot != NULL)
			{
				v->server = strndup(firstdot + 1, seconddot - firstdot - 1);
			}

			if (lastdot > seconddot)
			{
				v->backend = strndup(seconddot + 1, lastdot - seconddot - 1);
			}
		}
		else if (startparentheses != NULL && startparentheses < closeparentheses)
		{
			v->server = strndup(firstdot + 1, startparentheses - firstdot - 1);
			v->backend = strndup(startparentheses + 1, closeparentheses - startparentheses - 1);
		}
		else if (firstdot < lastdot)
		{
			v->backend = strndup(firstdot + 1, lastdot - firstdot - 1);
		}

		if (v->backend == NULL)
			v->backend = strdup("UNKNOWN");

		if (v->server == NULL)
			v->server = strdup("UNKNOWN");

		group_insert(pp, tmp, pt->sdesc, v);
		return 0;
	}

	char **target;

	// Create prefix
	strcat(tmp, "varnish_");
	strncat_lower(tmp, pt->name, firstdot - pt->name);

	if (lastunderscore != NULL && lastunderscore > lastdot)
	{
		// If we have a underscore then concat until the underscore
		strcat(tmp, "_");
		strncat(tmp, lastdot + 1, lastunderscore - lastdot - 1);

		for (g = groupings; g->prefix != NULL; g++)
		{
			if (strncmp(tmp, g->prefix, strlen(g->prefix)) == 0 && lastunderscore != NULL)
			{
				target = &v->type;
				*target = strdup(lastunderscore + 1);

				group_insert(pp, tmp, g->desc, v);
				return 0;
			}
		}

		strcat(tmp, lastunderscore);
	}
	else
	{
		// Just add everything from the last dot
		strcat(tmp, "_");
		strcat(tmp, lastdot + 1);
	}

	target = &v->id;

	for (identifier = target_indentifers; *identifier != NULL; identifier++)
	{
		if (strcmp(tmp, *identifier) == 0)
		{
			target = &v->target;
			break;
		}
	}

	for (identifier = type_indentifers; *identifier != NULL; identifier++)
	{
		if (strcmp(tmp, *identifier) == 0)
		{
			target = &v->type;
			break;
		}
	}

	if (firstdot < lastdot)
	{
		*target = strndup(firstdot + 1, lastdot - firstdot - 1);
	}

	group_insert(pp, tmp, pt->sdesc, v);

	return 0;
}

VCL_VOID
vmod_render(VRT_CTX, struct vmod_priv *priv)
{
	struct vsc *vsc;
	struct vsm *vd;

	struct prometheus_priv *p = priv->priv;
	ALLOC_OBJ(p, PROMETHEUS_PRIV_OBJECT_MAGIC);
	priv->methods = priv_vcl_method;

	VTAILQ_INIT(&p->groups);

	vsc = VSC_New();
	AN(vsc);

	vd = VSM_New();
	AN(vd);

	struct vsc *vsconce = VSC_New();
	CAST_OBJ_NOTNULL(p->vsb, ctx->specific, VSB_MAGIC);

	if (VSM_Attach(vd, STDERR_FILENO))
	{
		return;
	}

	AN(vsconce);
	AN(VSC_Arg(vsconce, 'f', "MAIN.uptime"));

	(void)VSC_Iter(vsconce, vd, do_once_cb_first, p);
	VSC_Destroy(&vsconce, vd);
	(void)VSC_Iter(vsc, vd, do_once_cb, p);
	VSC_Destroy(&vsc, vd);
	VSM_Destroy(&vd);

	synth_response(p);
}
