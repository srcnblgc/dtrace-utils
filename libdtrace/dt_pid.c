/*
 * Oracle Linux DTrace.
 * Copyright (c) 2010, 2018, Oracle and/or its affiliates. All rights reserved.
 * Licensed under the Universal Permissive License v 1.0 as shown at
 * http://oss.oracle.com/licenses/upl.
 */

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <alloca.h>
#include <libgen.h>
#include <stddef.h>
#include <sys/ioctl.h>

#include <mutex.h>
#include <port.h>

#include <dt_impl.h>
#include <dt_program.h>
#include <dt_pid.h>
#include <dt_string.h>

typedef struct dt_pid_probe {
	dtrace_hdl_t *dpp_dtp;
	dt_pcb_t *dpp_pcb;
	dt_proc_t *dpp_dpr;
	struct ps_prochandle *dpp_pr;
	const char *dpp_mod;
	char *dpp_func;
	const char *dpp_name;
	const char *dpp_obj;
	uintptr_t dpp_pc;
	size_t dpp_size;
	Lmid_t dpp_lmid;
	uint_t dpp_nmatches;
	uint64_t dpp_stret[4];
	GElf_Sym dpp_last;
	uint_t dpp_last_taken;
} dt_pid_probe_t;

/*
 * Compose the lmid and object name into the canonical representation. We
 * omit the lmid for the default link map for convenience.
 */
static void
dt_pid_objname(char *buf, size_t len, Lmid_t lmid, const char *obj)
{
	if (lmid == LM_ID_BASE)
		(void) strncpy(buf, obj, len);
	else
		(void) snprintf(buf, len, "LM%lx`%s", lmid, obj);
}

static int
dt_pid_error(dtrace_hdl_t *dtp, dt_pcb_t *pcb, dt_proc_t *dpr,
    fasttrap_probe_spec_t *ftp, dt_errtag_t tag, const char *fmt, ...)
{
	va_list ap;
	int len;

	if (ftp != NULL)
		dt_free(dtp, ftp);

	va_start(ap, fmt);
	if (pcb == NULL) {
		assert(dpr != NULL);
		len = vsnprintf(dpr->dpr_errmsg, sizeof (dpr->dpr_errmsg),
		    fmt, ap);
		assert(len >= 2);
		if (dpr->dpr_errmsg[len - 2] == '\n')
			dpr->dpr_errmsg[len - 2] = '\0';
	} else {
		dt_set_errmsg(dtp, dt_errtag(tag), pcb->pcb_region,
		    pcb->pcb_filetag, pcb->pcb_fileptr ? yylineno : 0, fmt, ap);
	}
	va_end(ap);

	return (1);
}

static int
dt_pid_create_fbt_probe(struct ps_prochandle *P, dtrace_hdl_t *dtp,
    fasttrap_probe_spec_t *ftp, const GElf_Sym *symp,
    fasttrap_probe_type_t type)
{
	ftp->ftps_type = type;
	ftp->ftps_pc = (uintptr_t)symp->st_value;
	ftp->ftps_size = (size_t)symp->st_size;
	ftp->ftps_glen = 0;		/* no glob pattern */
	ftp->ftps_gstr[0] = '\0';

	if (ioctl(dtp->dt_ftfd, FASTTRAPIOC_MAKEPROBE, ftp) != 0) {
		dt_dprintf("fasttrap probe creation ioctl failed: %s\n",
		    strerror(errno));
		return (dt_set_errno(dtp, errno));
	}

	return (1);
}

static int
dt_pid_create_glob_offset_probes(struct ps_prochandle *P, dtrace_hdl_t *dtp,
    fasttrap_probe_spec_t *ftp, const GElf_Sym *symp, const char *pattern)
{
	ftp->ftps_type = DTFTP_OFFSETS;
	ftp->ftps_pc = (uintptr_t)symp->st_value;
	ftp->ftps_size = (size_t)symp->st_size;
	ftp->ftps_glen = strlen(pattern);

	strncpy(ftp->ftps_gstr, pattern, ftp->ftps_glen + 1);

	if (ioctl(dtp->dt_ftfd, FASTTRAPIOC_MAKEPROBE, ftp) != 0) {
		dt_dprintf("fasttrap probe creation ioctl failed: %s\n",
		    strerror(errno));
		return (dt_set_errno(dtp, errno));
	}

	return (1);
}

static int
dt_pid_per_sym(dt_pid_probe_t *pp, const GElf_Sym *symp, const char *func)
{
	dtrace_hdl_t *dtp = pp->dpp_dtp;
	dt_pcb_t *pcb = pp->dpp_pcb;
	dt_proc_t *dpr = pp->dpp_dpr;
	fasttrap_probe_spec_t *ftp;
	uint64_t off;
	char *end;
	uint_t nmatches = 0;
	ulong_t sz;
	int glob;
	int isdash = strcmp("-", func) == 0;
	pid_t pid;

	/*
	 * We can just use the P member directly, since the PID does not change
	 * under exec().
	 */
	pid = Pgetpid(pp->dpp_pr);

	dt_dprintf("creating probe pid%d:%s:%s:%s at %lx\n", (int)pid,
	    pp->dpp_obj, func, pp->dpp_name, symp->st_value);

	sz = sizeof (fasttrap_probe_spec_t) + strlen(pp->dpp_name);

	if ((ftp = dt_alloc(dtp, sz)) == NULL) {
		dt_dprintf("proc_per_sym: dt_alloc(%lu) failed\n", sz);
		return (1); /* errno is set for us */
	}

	ftp->ftps_pid = pid;
	(void) strncpy(ftp->ftps_func, func, sizeof (ftp->ftps_func));

	dt_pid_objname(ftp->ftps_mod, sizeof (ftp->ftps_mod), pp->dpp_lmid,
	    pp->dpp_obj);

	if (!isdash && gmatch("return", pp->dpp_name)) {
		if (dt_pid_create_fbt_probe(pp->dpp_pr, dtp, ftp, symp,
		    DTFTP_RETURN) < 0) {
			return (dt_pid_error(dtp, pcb, dpr, ftp,
			    D_PROC_CREATEFAIL, "failed to create return probe "
			    "for '%s': %s", func,
			    dtrace_errmsg(dtp, dtrace_errno(dtp))));
		}

		nmatches++;
	}

	if (!isdash && gmatch("entry", pp->dpp_name)) {
		if (dt_pid_create_fbt_probe(pp->dpp_pr, dtp, ftp, symp,
		    DTFTP_ENTRY) < 0) {
			return (dt_pid_error(dtp, pcb, dpr, ftp,
			    D_PROC_CREATEFAIL, "failed to create entry probe "
			    "for '%s': %s", func,
			    dtrace_errmsg(dtp, dtrace_errno(dtp))));
		}

		nmatches++;
	}

	glob = strisglob(pp->dpp_name);
	if (!glob && nmatches == 0) {
		off = strtoull(pp->dpp_name, &end, 16);
		if (*end != '\0') {
			return (dt_pid_error(dtp, pcb, dpr, ftp, D_PROC_NAME,
			    "'%s' is an invalid probe name", pp->dpp_name));
		}

		if (off >= symp->st_size) {
			return (dt_pid_error(dtp, pcb, dpr, ftp, D_PROC_OFF,
			    "offset 0x%llx outside of function '%s'",
			    (u_longlong_t)off, func));
		}

		if (dt_pid_create_glob_offset_probes(pp->dpp_pr, pp->dpp_dtp,
		    ftp, symp, pp->dpp_name) < 0) {
			return (dt_pid_error(dtp, pcb, dpr, ftp,
			    D_PROC_CREATEFAIL, "failed to create probes at "
			    "'%s+0x%llx': %s", func, (u_longlong_t)off,
			    dtrace_errmsg(dtp, dtrace_errno(dtp))));
		}

		nmatches++;
	} else if (glob && !isdash) {
		if (dt_pid_create_glob_offset_probes(pp->dpp_pr, pp->dpp_dtp,
		    ftp, symp, pp->dpp_name) < 0) {
			return (dt_pid_error(dtp, pcb, dpr, ftp,
			    D_PROC_CREATEFAIL,
			    "failed to create offset probes in '%s': %s", func,
			    dtrace_errmsg(dtp, dtrace_errno(dtp))));
		}

		nmatches++;
	}

	pp->dpp_nmatches += nmatches;

	dt_free(dtp, ftp);

	return (0);
}

static int
dt_pid_sym_filt(void *arg, const GElf_Sym *symp, const char *func)
{
	dt_pid_probe_t *pp = arg;

	if (symp->st_shndx == SHN_UNDEF)
		return (0);

	if (symp->st_size == 0) {
		dt_dprintf("st_size of %s is zero\n", func);
		return (0);
	}

	if (pp->dpp_last_taken == 0 ||
	    symp->st_value != pp->dpp_last.st_value ||
	    symp->st_size != pp->dpp_last.st_size) {
		/*
		 * Due to 4524008, _init and _fini may have a bloated st_size.
		 * While this bug has been fixed for a while, old binaries
		 * may exist that still exhibit this problem. As a result, we
		 * don't match _init and _fini though we allow users to
		 * specify them explicitly.
		 */
		if (strcmp(func, "_init") == 0 || strcmp(func, "_fini") == 0)
			return (0);

		if ((pp->dpp_last_taken = gmatch(func, pp->dpp_func)) != 0) {
			pp->dpp_last = *symp;
			return (dt_pid_per_sym(pp, symp, func));
		}
	}

	return (0);
}

static int
dt_pid_per_mod(void *arg, const prmap_t *pmp, const char *obj)
{
	dt_pid_probe_t *pp = arg;
	dtrace_hdl_t *dtp = pp->dpp_dtp;
	dt_pcb_t *pcb = pp->dpp_pcb;
	dt_proc_t *dpr = pp->dpp_dpr;
	pid_t pid = Pgetpid(dpr->dpr_proc);
	GElf_Sym sym;

	if (obj == NULL)
		return (0);

	dt_Plmid(pp->dpp_dtp, pid, pmp->pr_vaddr, &pp->dpp_lmid);

	/*
	 * Note: if an execve() happens in the victim after this point, the
	 * following lookups will (unavoidably) fail if the lmid in the previous
	 * executable is not valid iun the new one.
	 */

	if ((pp->dpp_obj = strrchr(obj, '/')) == NULL)
		pp->dpp_obj = obj;
	else
		pp->dpp_obj++;

	if (dt_Pxlookup_by_name(pp->dpp_dtp, pid, pp->dpp_lmid, obj, ".stret1",
		&sym, NULL) == 0)
		pp->dpp_stret[0] = sym.st_value;
	else
		pp->dpp_stret[0] = 0;

	if (dt_Pxlookup_by_name(pp->dpp_dtp, pid, pp->dpp_lmid, obj, ".stret2",
		&sym, NULL) == 0)
		pp->dpp_stret[1] = sym.st_value;
	else
		pp->dpp_stret[1] = 0;

	if (dt_Pxlookup_by_name(pp->dpp_dtp, pid, pp->dpp_lmid, obj, ".stret4",
		&sym, NULL) == 0)
		pp->dpp_stret[2] = sym.st_value;
	else
		pp->dpp_stret[2] = 0;

	if (dt_Pxlookup_by_name(pp->dpp_dtp, pid, pp->dpp_lmid, obj, ".stret8",
		&sym, NULL) == 0)
		pp->dpp_stret[3] = sym.st_value;
	else
		pp->dpp_stret[3] = 0;

	dt_dprintf("%s stret %llx %llx %llx %llx\n", obj,
	    (u_longlong_t)pp->dpp_stret[0], (u_longlong_t)pp->dpp_stret[1],
	    (u_longlong_t)pp->dpp_stret[2], (u_longlong_t)pp->dpp_stret[3]);

	/*
	 * If pp->dpp_func contains any globbing meta-characters, we need
	 * to iterate over the symbol table and compare each function name
	 * against the pattern.
	 */
	if (!strisglob(pp->dpp_func)) {
		/*
		 * If we fail to lookup the symbol, try interpreting the
		 * function as the special "-" function that indicates that the
		 * probe name should be interpreted as a absolute virtual
		 * address. If that fails and we were matching a specific
		 * function in a specific module, report the error, otherwise
		 * just fail silently in the hopes that some other object will
		 * contain the desired symbol.
		 */
		if (dt_Pxlookup_by_name(pp->dpp_dtp, pid, pp->dpp_lmid, obj,
			pp->dpp_func, &sym, NULL) != 0) {
			if (strcmp("-", pp->dpp_func) == 0) {
				sym.st_name = 0;
				sym.st_info =
				    GELF_ST_INFO(STB_LOCAL, STT_FUNC);
				sym.st_other = 0;
				sym.st_value = 0;
				sym.st_size = Pelf64(pp->dpp_pr) ? -1ULL : -1U;
			} else if (!strisglob(pp->dpp_mod)) {
				return (dt_pid_error(dtp, pcb, dpr, NULL,
				    D_PROC_FUNC,
				    "failed to lookup '%s' in module '%s'",
				    pp->dpp_func, pp->dpp_mod));
			} else {
				return (0);
			}
		}

		/*
		 * Only match defined functions of non-zero size.
		 */
		if (GELF_ST_TYPE(sym.st_info) != STT_FUNC ||
		    sym.st_shndx == SHN_UNDEF || sym.st_size == 0)
			return (0);

		/*
		 * We don't instrument writable mappings such as PLTs -- they're
		 * dynamically rewritten, and, so, inherently dicey to
		 * instrument.
		 */
		if (dt_Pwritable_mapping(pp->dpp_dtp, pid, sym.st_value))
			return (0);

		dt_Plookup_by_addr(pp->dpp_dtp, pid, sym.st_value,
		    pp->dpp_func, DTRACE_FUNCNAMELEN, &sym);

		return (dt_pid_per_sym(pp, &sym, pp->dpp_func));
	} else {
		uint_t nmatches = pp->dpp_nmatches;

		if (dt_Psymbol_iter_by_addr(pp->dpp_dtp, pid, obj, PR_SYMTAB,
			BIND_ANY | TYPE_FUNC, dt_pid_sym_filt, pp) == 1)
			return (1);

		if (nmatches == pp->dpp_nmatches) {
			/*
			 * If we didn't match anything in the PR_SYMTAB, try
			 * the PR_DYNSYM.
			 */
			if (dt_Psymbol_iter_by_addr(pp->dpp_dtp, pid, obj,
				PR_DYNSYM, BIND_ANY | TYPE_FUNC,
				dt_pid_sym_filt, pp) == 1)
				return (1);
		}
	}

	return (0);
}

static int
dt_pid_mod_filt(void *arg, const prmap_t *pmp, const char *obj)
{
	char name[DTRACE_MODNAMELEN];
	dt_pid_probe_t *pp = arg;
	dt_proc_t *dpr = pp->dpp_dpr;

	if ((pp->dpp_obj = strrchr(obj, '/')) == NULL)
		pp->dpp_obj = obj;
	else
		pp->dpp_obj++;

	if (gmatch(pp->dpp_obj, pp->dpp_mod))
		return (dt_pid_per_mod(pp, pmp, obj));

	dt_Plmid(pp->dpp_dtp, Pgetpid(dpr->dpr_proc), pmp->pr_vaddr,
		 &pp->dpp_lmid);

	dt_pid_objname(name, sizeof (name), pp->dpp_lmid, pp->dpp_obj);

	if (gmatch(name, pp->dpp_mod))
		return (dt_pid_per_mod(pp, pmp, obj));

	return (0);
}

static const prmap_t *
dt_pid_fix_mod(dtrace_probedesc_t *pdp, dtrace_hdl_t *dtp, pid_t pid)
{
	char m[PATH_MAX];
	Lmid_t lmid = PR_LMID_EVERY;
	const char *obj;
	const prmap_t *pmp;

	/*
	 * Pick apart the link map from the library name.
	 */
	if (strchr(pdp->dtpd_mod, '`') != NULL) {
		char *end;

		if (strncmp(pdp->dtpd_mod, "LM", 2) != 0 ||
		    !isdigit(pdp->dtpd_mod[2]))
			return (NULL);

		lmid = strtoul(&pdp->dtpd_mod[2], &end, 16);

		obj = end + 1;

		if (*end != '`' || strchr(obj, '`') != NULL)
			return (NULL);

	} else {
		obj = pdp->dtpd_mod;
	}

	if ((pmp = dt_Plmid_to_map(dtp, pid, lmid, obj)) == NULL)
		return (NULL);

	dt_Pobjname(dtp, pid, pmp->pr_vaddr, m, sizeof (m));
	if ((obj = strrchr(m, '/')) == NULL)
		obj = &m[0];
	else
		obj++;

	dt_Plmid(dtp, pid, pmp->pr_vaddr, &lmid);
	dt_pid_objname(pdp->dtpd_mod, sizeof (pdp->dtpd_mod), lmid, obj);

	return (pmp);
}

static int
dt_pid_create_pid_probes(dtrace_probedesc_t *pdp, dtrace_hdl_t *dtp,
    dt_pcb_t *pcb, dt_proc_t *dpr)
{
	dt_pid_probe_t pp;
	int ret = 0;
	pid_t pid = Pgetpid(dpr->dpr_proc);

	pp.dpp_dtp = dtp;
	pp.dpp_dpr = dpr;
	pp.dpp_pr = dpr->dpr_proc;
	pp.dpp_pcb = pcb;
	pp.dpp_nmatches = 0;

	/*
	 * We can only trace dynamically-linked executables (since we've
	 * hidden some magic in ld.so.1 as well as libc.so.1).
	 */
	if (dt_Pname_to_map(dtp, pid, PR_OBJ_LDSO) == NULL) {
		return (dt_pid_error(dtp, pcb, dpr, NULL, D_PROC_DYN,
		    "process %s is not a dynamically-linked executable",
		    &pdp->dtpd_provider[3]));
	}

	pp.dpp_mod = pdp->dtpd_mod[0] != '\0' ? pdp->dtpd_mod : "*";
	pp.dpp_func = pdp->dtpd_func[0] != '\0' ? pdp->dtpd_func : "*";
	pp.dpp_name = pdp->dtpd_name[0] != '\0' ? pdp->dtpd_name : "*";
	pp.dpp_last_taken = 0;

	if (strcmp(pp.dpp_func, "-") == 0) {
		const prmap_t *aout, *pmp;

		if (pdp->dtpd_mod[0] == '\0') {
			pp.dpp_mod = pdp->dtpd_mod;
			(void) strcpy(pdp->dtpd_mod, "a.out");
		} else if (strisglob(pp.dpp_mod) ||
		    (aout = dt_Pname_to_map(dtp, pid, "a.out")) == NULL ||
		    (pmp = dt_Pname_to_map(dtp, pid, pp.dpp_mod)) == NULL ||
		    aout->pr_vaddr != pmp->pr_vaddr) {
			return (dt_pid_error(dtp, pcb, dpr, NULL, D_PROC_LIB,
			    "only the a.out module is valid with the "
			    "'-' function"));
		}

		if (strisglob(pp.dpp_name)) {
			return (dt_pid_error(dtp, pcb, dpr, NULL, D_PROC_NAME,
			    "only individual addresses may be specified "
			    "with the '-' function"));
		}
	}

	/*
	 * If pp.dpp_mod contains any globbing meta-characters, we need
	 * to iterate over each module and compare its name against the
	 * pattern. An empty module name is treated as '*'.
	 */
	if (strisglob(pp.dpp_mod)) {
		ret = dt_Pobject_iter(dtp, pid, dt_pid_mod_filt, &pp);
	} else {
		const prmap_t *pmp;
		char *obj;

		/*
		 * If we can't find a matching module, don't sweat it -- either
		 * we'll fail the enabling because the probes don't exist or
		 * we'll wait for that module to come along.
		 */
		if ((pmp = dt_pid_fix_mod(pdp, dtp, pid)) != NULL) {
			if ((obj = strchr(pdp->dtpd_mod, '`')) == NULL)
				obj = pdp->dtpd_mod;
			else
				obj++;

			ret = dt_pid_per_mod(&pp, pmp, obj);
		}
	}

	return (ret);
}

static int
dt_pid_usdt_mapping(void *data, const prmap_t *pmp, const char *oname)
{
	dt_proc_t *dpr = data;
	GElf_Sym sym;
	prsyminfo_t sip;
	dof_helper_t dh;
	GElf_Half e_type;
	const char *mname;
	const char *syms[] = { "___SUNW_dof", "__SUNW_dof" };
	int i;
#if 0
	int fd = -1;
#endif

	/*
	 * The symbol ___SUNW_dof is for lazy-loaded DOF sections, and
	 * __SUNW_dof is for actively-loaded DOF sections. We try to force
	 * in both types of DOF section since the process may not yet have
	 * run the code to instantiate these providers.
	 */
	for (i = 0; i < 2; i++) {
		if (dt_Pxlookup_by_name(dpr->dpr_hdl, dpr->dpr_pid, PR_LMID_EVERY,
			oname, syms[i], &sym, &sip) != 0) {
			continue;
		}

		if ((mname = strrchr(oname, '/')) == NULL)
			mname = oname;
		else
			mname++;

		dt_dprintf("lookup of %s succeeded for %s\n", syms[i], mname);

		if (dt_Pread(dpr->dpr_hdl, dpr->dpr_pid, &e_type,
			sizeof (e_type), pmp->pr_vaddr + offsetof(Elf64_Ehdr,
			    e_type)) != sizeof (e_type)) {
			dt_dprintf("read of ELF header failed");
			continue;
		}

		dh.dofhp_dof = sym.st_value;
		dh.dofhp_addr = (e_type == ET_EXEC) ? 0 : pmp->pr_vaddr;

		dt_pid_objname(dh.dofhp_mod, sizeof (dh.dofhp_mod),
		    sip.prs_lmid, mname);
#if 0
		if (fd == -1 &&
		    (fd = pr_open(dpr->dpr_proc, "/dev/dtrace/helper", O_RDWR, 0)) < 0) {
			dt_dprintf("pr_open of helper device failed: %s\n",
			    strerror(errno));
			return (-1); /* errno is set for us */
		}

		if (pr_ioctl(P.P, fd, DTRACEHIOC_ADDDOF, &dh, sizeof (dh)) < 0)
			dt_dprintf("DOF was rejected for %s\n", dh.dofhp_mod);
#endif
	}
#if 0
	if (fd != -1)
		(void) pr_close(P.P, fd);
#endif

	return (0);
}

static int
dt_pid_create_usdt_probes(dtrace_probedesc_t *pdp, dtrace_hdl_t *dtp,
    dt_pcb_t *pcb, dt_proc_t *dpr)
{
	int ret = 0;

	assert(MUTEX_HELD(&dpr->dpr_lock));

	if (dt_Pobject_iter(dtp, dpr->dpr_pid, dt_pid_usdt_mapping, dpr) != 0) {
		ret = -1;
		dt_pid_error(dtp, pcb, dpr, NULL, D_PROC_USDT,
		    "failed to instantiate probes for pid %d: %s",
		    dpr->dpr_pid, strerror(errno));
	}

	/*
	 * Put the module name in its canonical form.
	 */
	dt_pid_fix_mod(pdp, dtp, dpr->dpr_pid);

	return (ret);
}

static pid_t
dt_pid_get_pid(dtrace_probedesc_t *pdp, dtrace_hdl_t *dtp, dt_pcb_t *pcb,
    dt_proc_t *dpr)
{
	pid_t pid;
	char *c, *last = NULL, *end;

	for (c = &pdp->dtpd_provider[0]; *c != '\0'; c++) {
		if (!isdigit(*c))
			last = c;
	}

	if (last == NULL || (*(++last) == '\0')) {
		(void) dt_pid_error(dtp, pcb, dpr, NULL, D_PROC_BADPROV,
		    "'%s' is not a valid provider", pdp->dtpd_provider);
		return (-1);
	}

	errno = 0;
	pid = strtol(last, &end, 10);

	if (errno != 0 || end == last || end[0] != '\0' || pid <= 0) {
		(void) dt_pid_error(dtp, pcb, dpr, NULL, D_PROC_BADPID,
		    "'%s' does not contain a valid pid", pdp->dtpd_provider);
		return (-1);
	}

	return (pid);
}

int
dt_pid_create_probes(dtrace_probedesc_t *pdp, dtrace_hdl_t *dtp, dt_pcb_t *pcb)
{
	char provname[DTRACE_PROVNAMELEN];
	dt_proc_t *dpr;
	pid_t pid;
	int err = 0;

	assert(pcb != NULL);

	if ((pid = dt_pid_get_pid(pdp, dtp, pcb, NULL)) == -1)
		return (-1);

	if (dtp->dt_ftfd == -1) {
		if (dtp->dt_fterr == ENOENT) {
			(void) dt_pid_error(dtp, pcb, NULL, NULL, D_PROC_NODEV,
			    "pid provider is not installed on this system");
		} else {
			(void) dt_pid_error(dtp, pcb, NULL, NULL, D_PROC_NODEV,
			    "pid provider is not available: %s",
			    strerror(dtp->dt_fterr));
		}

		return (-1);
	}

	(void) snprintf(provname, sizeof (provname), "pid%d", (int)pid);

	if (gmatch(provname, pdp->dtpd_provider) != 0) {
		pid = dt_proc_grab_lock(dtp, pid, DTRACE_PROC_WAITING);
		if (pid < 0) {
			dt_pid_error(dtp, pcb, NULL, NULL, D_PROC_GRAB,
			    "failed to grab process %d", (int)pid);
			return (-1);
		}

		dpr = dt_proc_lookup(dtp, pid);
		assert(dpr != NULL);

		if ((err = dt_pid_create_pid_probes(pdp, dtp, pcb, dpr)) == 0) {
			/*
			 * Alert other retained enablings which may match
			 * against the newly created probes.
			 */
			dt_ioctl(dtp, DTRACEIOC_ENABLE, NULL);
		}

		dt_proc_release_unlock(dtp, pid);
	}

	/*
	 * If it's not strictly a pid provider, we might match a USDT provider.
	 */
	if (strcmp(provname, pdp->dtpd_provider) != 0) {
		pid = dt_proc_grab_lock(dtp, pid, DTRACE_PROC_WAITING);
		if (pid < 0) {
			dt_pid_error(dtp, pcb, NULL, NULL, D_PROC_GRAB,
			    "failed to grab process %d", (int)pid);
			return (-1);
		}

		dpr = dt_proc_lookup(dtp, pid);
		assert(dpr != NULL);

		if (!dpr->dpr_usdt) {
			err = dt_pid_create_usdt_probes(pdp, dtp, pcb, dpr);
			dpr->dpr_usdt = B_TRUE;
		}

		dt_proc_release_unlock(dtp, pid);
	}

	return (err ? -1 : 0);
}

int
dt_pid_create_probes_module(dtrace_hdl_t *dtp, dt_proc_t *dpr)
{
	dtrace_prog_t *pgp;
	dt_stmt_t *stp;
	dtrace_probedesc_t *pdp, pd;
	pid_t pid;
	int ret = 0, found = B_FALSE;
	char provname[DTRACE_PROVNAMELEN];

	(void) snprintf(provname, sizeof (provname), "pid%d",
	    (int)dpr->dpr_pid);

	for (pgp = dt_list_next(&dtp->dt_programs); pgp != NULL;
	    pgp = dt_list_next(pgp)) {

		for (stp = dt_list_next(&pgp->dp_stmts); stp != NULL;
		    stp = dt_list_next(stp)) {

			pdp = &stp->ds_desc->dtsd_ecbdesc->dted_probe;
			pid = dt_pid_get_pid(pdp, dtp, NULL, dpr);
			if (pid != dpr->dpr_pid)
				continue;

			found = B_TRUE;

			pd = *pdp;

			if (gmatch(provname, pdp->dtpd_provider) != 0 &&
			    dt_pid_create_pid_probes(&pd, dtp, NULL, dpr) != 0)
				ret = 1;

			/*
			 * If it's not strictly a pid provider, we might match
			 * a USDT provider.
			 */
			if (strcmp(provname, pdp->dtpd_provider) != 0 &&
			    dt_pid_create_usdt_probes(&pd, dtp, NULL, dpr) != 0)
				ret = 1;
		}
	}

	if (found) {
		/*
		 * Give DTrace a shot to the ribs to get it to check
		 * out the newly created probes.
		 */
		(void) dt_ioctl(dtp, DTRACEIOC_ENABLE, NULL);
	}

	return (ret);
}
