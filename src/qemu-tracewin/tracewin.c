/*
 * tracewin: QEMU TCG plugin that logs executed instructions between two
 * trace markers, and runs at close to full speed outside them.
 *
 *   trace on:  addi x0, x0, 0x5a1   (0x5a100013)
 *   trace off: addi x0, x0, 0x5a0   (0x5a000013)
 *
 * Both are RISC-V HINTs (architectural no-ops), so the markers can stay in
 * code that also runs on Spike (with the custom_rv32 patch), in RTL sim or
 * on hardware. Outside the window only an inline condition runs per
 * instruction; memory callbacks still run but return immediately.
 *
 * Output goes to QEMU's log (-d plugin -D <file>), one line per instruction:
 *
 *   0x80001234 (0x00a12023) sw a0,0(sp) store 0x80ffff00 0x00000005
 *   0x80001238 (0x00450513) addi a0,a0,4 a0=0x00000009
 *
 * Options (-plugin libtracewin.so,opt=val,...):
 *   all=on     trace from reset, ignore the markers
 *   regs=on    append integer registers changed by each instruction (slower)
 *   mem=off    don't log loads and stores
 *   max=N      stop after N traced instructions
 */
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define MARK_ON  0x5a100013u
#define MARK_OFF 0x5a000013u
#define MAX_VCPUS 64

typedef struct {
	uint64_t vaddr;
	uint32_t bits;
	int size;
	char *disas;
} Insn;

typedef struct {
	struct qemu_plugin_register *handle;
	const char *name;
	GByteArray *last, *cur;
} Reg;

typedef struct {
	GString *line;
	GPtrArray *regs;
} Cpu;

static Cpu cpus[MAX_VCPUS];
static struct qemu_plugin_scoreboard *score;
static qemu_plugin_u64 tracing;
static bool opt_all, opt_regs, opt_mem = true;
static uint64_t opt_max, traced;
static bool done;

static void reg_diffs(Cpu *c, bool print)
{
	for (guint i = 0; i < c->regs->len; i++) {
		Reg *r = c->regs->pdata[i];
		GByteArray *t;

		g_byte_array_set_size(r->cur, 0);
		if (!qemu_plugin_read_register(r->handle, r->cur))
			continue;
		if (print && (r->cur->len != r->last->len ||
			      memcmp(r->cur->data, r->last->data, r->cur->len))) {
			g_string_append_printf(c->line, " %s=0x", r->name);
			for (int b = r->cur->len - 1; b >= 0; b--)
				g_string_append_printf(c->line, "%02x", r->cur->data[b]);
		}
		t = r->last;
		r->last = r->cur;
		r->cur = t;
	}
}

static void flush(Cpu *c)
{
	if (!c->line->len)
		return;
	g_string_append_c(c->line, '\n');
	qemu_plugin_outs(c->line->str);
	g_string_truncate(c->line, 0);
}

static void insn_exec(unsigned int vcpu, void *udata)
{
	Cpu *c = &cpus[vcpu];
	Insn *d = udata;

	if (c->line->len) {
		if (opt_regs)
			reg_diffs(c, true);
		flush(c);
	}
	g_string_printf(c->line, "0x%08" PRIx64 " (0x%0*x) %s",
			d->vaddr, d->size * 2, d->bits, d->disas);
	if (opt_max && ++traced >= opt_max) {
		done = true;
		qemu_plugin_u64_set(tracing, vcpu, 0);
	}
}

static void mark_on(unsigned int vcpu, void *udata)
{
	if (done)
		return;
	if (opt_regs)
		reg_diffs(&cpus[vcpu], false);	/* start diffs from here */
	qemu_plugin_u64_set(tracing, vcpu, 1);
}

static void mark_off(unsigned int vcpu, void *udata)
{
	flush(&cpus[vcpu]);
	qemu_plugin_u64_set(tracing, vcpu, 0);
}

static void mem_access(unsigned int vcpu, qemu_plugin_meminfo_t info,
		       uint64_t vaddr, void *udata)
{
	qemu_plugin_mem_value v;
	uint64_t val;
	int width;

	if (!qemu_plugin_u64_get(tracing, vcpu))
		return;
	v = qemu_plugin_mem_get_value(info);
	switch (v.type) {
	case QEMU_PLUGIN_MEM_VALUE_U8:  val = v.data.u8;  width = 2;  break;
	case QEMU_PLUGIN_MEM_VALUE_U16: val = v.data.u16; width = 4;  break;
	case QEMU_PLUGIN_MEM_VALUE_U32: val = v.data.u32; width = 8;  break;
	case QEMU_PLUGIN_MEM_VALUE_U64: val = v.data.u64; width = 16; break;
	default:			val = v.data.u128.low; width = 16; break;
	}
	g_string_append_printf(cpus[vcpu].line, " %s 0x%08" PRIx64 " 0x%0*" PRIx64,
			       qemu_plugin_mem_is_store(info) ? "store" : "load",
			       vaddr, width, val);
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
	enum qemu_plugin_cb_flags rflags =
		opt_regs ? QEMU_PLUGIN_CB_R_REGS : QEMU_PLUGIN_CB_NO_REGS;
	size_t n = qemu_plugin_tb_n_insns(tb);

	for (size_t i = 0; i < n; i++) {
		struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
		Insn *d = g_new0(Insn, 1);

		d->vaddr = qemu_plugin_insn_vaddr(insn);
		d->size = qemu_plugin_insn_size(insn);
		qemu_plugin_insn_data(insn, &d->bits, MIN(d->size, 4));
		d->disas = qemu_plugin_insn_disas(insn);

		/* callbacks run in registration order, before the insn executes */
		if (!opt_all && d->bits == MARK_ON)
			qemu_plugin_register_vcpu_insn_exec_cb(insn, mark_on, rflags, NULL);
		qemu_plugin_register_vcpu_insn_exec_cond_cb(insn, insn_exec, rflags,
							    QEMU_PLUGIN_COND_EQ,
							    tracing, 1, d);
		if (!opt_all && d->bits == MARK_OFF)
			qemu_plugin_register_vcpu_insn_exec_cb(insn, mark_off,
							       QEMU_PLUGIN_CB_NO_REGS, NULL);
		if (opt_mem)
			qemu_plugin_register_vcpu_mem_cb(insn, mem_access,
							 QEMU_PLUGIN_CB_NO_REGS,
							 QEMU_PLUGIN_MEM_RW, d);
	}
}

static void vcpu_init(qemu_plugin_id_t id, unsigned int vcpu)
{
	Cpu *c = &cpus[vcpu];

	g_assert(vcpu < MAX_VCPUS);
	c->line = g_string_new(NULL);
	c->regs = g_ptr_array_new();
	if (opt_regs) {
		g_autoptr(GArray) list = qemu_plugin_get_registers();

		for (guint i = 0; i < list->len; i++) {
			qemu_plugin_reg_descriptor *rd =
				&g_array_index(list, qemu_plugin_reg_descriptor, i);
			Reg *r;

			if (g_strcmp0(rd->feature, "org.gnu.gdb.riscv.cpu") ||
			    !g_strcmp0(rd->name, "pc"))
				continue;
			r = g_new0(Reg, 1);
			r->handle = rd->handle;
			r->name = g_intern_string(rd->name);
			r->last = g_byte_array_new();
			r->cur = g_byte_array_new();
			qemu_plugin_read_register(r->handle, r->last);
			g_ptr_array_add(c->regs, r);
		}
	}
	qemu_plugin_u64_set(tracing, vcpu, opt_all ? 1 : 0);
}

static void at_exit(qemu_plugin_id_t id, void *p)
{
	for (int i = 0; i < MAX_VCPUS; i++)
		if (cpus[i].line)
			flush(&cpus[i]);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
					   const qemu_info_t *info,
					   int argc, char **argv)
{
	for (int i = 0; i < argc; i++) {
		g_auto(GStrv) kv = g_strsplit(argv[i], "=", 2);
		bool ok = kv[1] != NULL;

		if (ok && !g_strcmp0(kv[0], "all"))
			ok = qemu_plugin_bool_parse(kv[0], kv[1], &opt_all);
		else if (ok && !g_strcmp0(kv[0], "regs"))
			ok = qemu_plugin_bool_parse(kv[0], kv[1], &opt_regs);
		else if (ok && !g_strcmp0(kv[0], "mem"))
			ok = qemu_plugin_bool_parse(kv[0], kv[1], &opt_mem);
		else if (ok && !g_strcmp0(kv[0], "max"))
			opt_max = g_ascii_strtoull(kv[1], NULL, 0);
		else
			ok = false;
		if (!ok) {
			fprintf(stderr, "tracewin: bad option '%s' "
				"(all=on|off, regs=on|off, mem=on|off, max=N)\n", argv[i]);
			return -1;
		}
	}

	score = qemu_plugin_scoreboard_new(sizeof(uint64_t));
	tracing = qemu_plugin_scoreboard_u64(score);
	qemu_plugin_register_vcpu_init_cb(id, vcpu_init);
	qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
	qemu_plugin_register_atexit_cb(id, at_exit, NULL);
	return 0;
}
