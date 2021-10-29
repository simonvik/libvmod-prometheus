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
	VTAILQ_HEAD(, prometheus_group)
	groups;
	struct vsb *vsb;
	struct ws *ws;
};

struct prometheus_group
{
	VTAILQ_ENTRY(prometheus_group)
	list;
	VTAILQ_HEAD(, prometheus_value)
	v;
	char *group_name;
	char *description;
};

struct prometheus_value
{
	VTAILQ_ENTRY(prometheus_value)
	list;
	double val;
	char *id;
	char *server;
	char *backend;
	char *target;
	char *type;
};

static char *cleaup_backend_name(char *backend)
{
	if (strncmp(backend, "boot.", 5) == 0 || strncmp(backend, "root:", 5) == 0)
		return backend + 5;

	return backend;
}

static void synth_response(struct prometheus_priv *p)
{
	struct prometheus_group *k_item;
	struct prometheus_value *v_item;

	VTAILQ_FOREACH(k_item, &p->groups, list)
	{

		VSB_printf(p->vsb, "# HELP %s %s\n", k_item->group_name, k_item->description);
		VSB_printf(p->vsb, "# TYPE %s gauge\n", k_item->group_name);
		VTAILQ_FOREACH(v_item, &k_item->v, list)
		{
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
		}
	}
}

static void group_insert(struct prometheus_priv *p, char *group_name, const char *description, struct prometheus_value *v)
{
	struct prometheus_group *iter_group = NULL;
	struct prometheus_group *group = NULL;

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
		group = WS_Alloc(p->ws, sizeof(struct prometheus_group));
		group->description = WS_Copy(p->ws, description, strlen(description) + 1);
		group->group_name = WS_Copy(p->ws, group_name, strlen(group_name) + 1);

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

static int v_matchproto_(VSC_iter_f)
	do_once_cb(void *priv, const struct VSC_point *const pt)
{
	// It tries to copy prometheus.go in some way.


	const char *p;
	const char *firstdot = NULL;
	const char *lastdot = NULL;
	struct prometheus_value *v;

	// Hack hack, used for VBE
	const char *seconddot = NULL;

	const char *startparentheses = NULL;
	const char *closeparentheses = NULL;

	struct prometheus_priv *pp = priv;

	//Hold temporary name, this might be superunsafe
	char tmp[300] = {0};

	if(pt == NULL)
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
	}

	v = WS_Alloc(pp->ws, sizeof(struct prometheus_value));

	if (pt->name[0] == 'V' && pt->name[1] == 'B' && pt->name[2] == 'E')
	{
		v->server = "UNKNOWN";
		v->backend = "UNKNOWN";

		strcat(tmp, "varnish_backend_");
		strcat(tmp, lastdot + 1);

		if (startparentheses == NULL && seconddot > firstdot)
		{
			if (seconddot != NULL)
			{
				v->server = WS_Alloc(pp->ws, seconddot - firstdot);
				strncpy(v->server, firstdot + 1, seconddot - firstdot - 1);
			}

			if (lastdot > seconddot)
			{
				v->backend = WS_Alloc(pp->ws, lastdot - seconddot);
				strncpy(v->backend, seconddot + 1, lastdot - seconddot - 1);
			}
		}
		else if (startparentheses != NULL && startparentheses < closeparentheses)
		{
			v->server = WS_Alloc(pp->ws, startparentheses - firstdot);
			strncpy(v->server, firstdot + 1, startparentheses - firstdot - 1);

			v->backend = WS_Alloc(pp->ws, closeparentheses - startparentheses);
			strncpy(v->backend, startparentheses + 1, closeparentheses - startparentheses - 1);
		}
		else if (firstdot < lastdot)
		{
			v->backend = WS_Alloc(pp->ws, lastdot - firstdot);
			strncpy(v->backend, firstdot + 1, lastdot - firstdot - 1);
		}

		group_insert(pp, tmp, pt->sdesc, v);
	}
	else
	{
		char **target;

		strcat(tmp, "varnish_");
		strncat_lower(tmp, pt->name, firstdot - pt->name);
		strcat(tmp, "_");
		strcat(tmp, lastdot + 1);

		target = &v->id;

		if (strcmp(tmp, "varnish_lock_collisions") == 0 ||
			strcmp(tmp, "varnish_lock_created") == 0 ||
			strcmp(tmp, "varnish_lock_destroyed") == 0 ||
			strcmp(tmp, "varnish_lock_operations") == 0)
		{
			target = &v->target;
		}

		if (strcmp(tmp, "varnish_sma_c_bytes") == 0 ||
			strcmp(tmp, "varnish_sma_c_fail") == 0 ||
			strcmp(tmp, "varnish_sma_c_freed") == 0 ||
			strcmp(tmp, "varnish_sma_c_req") == 0 ||
			strcmp(tmp, "varnish_sma_g_alloc") == 0 ||
			strcmp(tmp, "varnish_sma_g_bytes") == 0 ||
			strcmp(tmp, "varnish_sma_g_space") == 0 ||
			strcmp(tmp, "varnish_smf_c_bytes") == 0 ||
			strcmp(tmp, "varnish_smf_c_fail") == 0 ||
			strcmp(tmp, "varnish_smf_c_freed") == 0 ||
			strcmp(tmp, "varnish_smf_c_req") == 0 ||
			strcmp(tmp, "varnish_smf_g_alloc") == 0 ||
			strcmp(tmp, "varnish_smf_g_bytes") == 0 ||
			strcmp(tmp, "varnish_smf_g_smf_frag") == 0 ||
			strcmp(tmp, "varnish_smf_g_smf_large") == 0 ||
			strcmp(tmp, "varnish_smf_g_smf") == 0 ||
			strcmp(tmp, "varnish_smf_g_space") == 0)
		{
			target = &v->type;
		}

		if (firstdot != lastdot)
		{
			*target = WS_Alloc(pp->ws, lastdot - firstdot);
			strncpy(*target, firstdot + 1, lastdot - firstdot - 1);
		}
		else
			*target = NULL;

		v->val = (double)VSC_Value(pt);

		group_insert(pp, tmp, pt->sdesc, v);
	}

	return 0;
}

VCL_VOID
vmod_render(VRT_CTX)
{
	struct vsc *vsc;
	struct vsm *vd;
	struct prometheus_priv p = {};

	VTAILQ_INIT(&p.groups);
	p.ws = ctx->ws;

	CHECK_OBJ_NOTNULL(p.ws, WS_MAGIC);

	vsc = VSC_New();
	AN(vsc);

	vd = VSM_New();
	AN(vd);

	struct vsc *vsconce = VSC_New();

	CAST_OBJ_NOTNULL(p.vsb, ctx->specific, VSB_MAGIC);

	if (VSM_Attach(vd, STDERR_FILENO))
	{
		return;
	}

	AN(vsconce);
	AN(VSC_Arg(vsconce, 'f', "MAIN.uptime"));

	(void)VSC_Iter(vsc, vd, do_once_cb, &p);

	synth_response(&p);
}
