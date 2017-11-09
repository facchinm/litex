# This file is Copyright (c) 2017 William D. Jones <thor0505@comcast.net>
# License: BSD

import os
import sys
import subprocess

from litex.gen.fhdl.structure import _Fragment

from litex.build.generic_platform import *
from litex.build import tools
from litex.build.lattice import common

def _format_constraint(c):
    pass


def _format_pcf(signame, pin, others, resname):
    return "set_io " + signame + " " + pin + "\n"


def _build_pcf(named_sc, named_pc):
    r = ""
    for sig, pins, others, resname in named_sc:
        if len(pins) > 1:
            for i, p in enumerate(pins):
                r += _format_pcf(sig + "[" + str(i) + "]", p, others, resname)
        else:
            r += _format_pcf(sig, pins[0], others, resname)
    if named_pc:
        r += "\n" + "\n\n".join(named_pc)
    return r


def _build_yosys(device, sources, vincpaths, build_name, pre_synth,
                 synth_override, post_synth):
    # Unconditionally-required yosys commands in the context of Migen.
    ys_contents = list()
    incflags = ""
    for path in vincpaths:
        incflags += " -I" + path
    for filename, language, library in sources:
        ys_contents.append("read_{}{} {}".format(language, incflags, filename))

    # Migen only outputs Xilinx-style attributes enclosed in strings.
    # (i.e. "true", "0", etc). yosys wants constant literals to represent
    # true and false, so convert before synthesis.
    ys_contents.append("attrmap -tocase keep -imap keep=\"true\" keep=1 -imap keep=\"false\" keep=0 -remove keep=0")

    # Pre-synthesis user-commands go here.
    for pre in pre_synth:
        ys_contents.append(pre.format(build_name=build_name))

    # Give the programmer an opportunity to override the default synthesis
    # command if they need to. The default is fine for most applications.
    # Must generate {build_name}.blif as output.
    if not synth_override:
        ys_contents.append("""synth_ice40 -top top -blif {build_name}.blif""".format(
            build_name=build_name))
    else:
        for synth in synth_override:
            ys_contents.append(synth.format(build_name=build_name))

    # Finally, add post-synthesis commands.
    for post in post_synth:
        ys_contents.append(post.format(build_name=build_name))

    ys_name = build_name + ".ys"
    tools.write_to_file(ys_name, "\n".join(ys_contents))


def _run_icestorm(build_name, source, yosys_opt, pnr_opt,
                  icetime_opt, icepack_opt):
    if sys.platform == "win32" or sys.platform == "cygwin":
        script_ext = ".bat"
        shell = ["cmd", "/c"]
        build_script_contents = "@echo off\nrem Autogenerated by LiteX\n"
        fail_stmt = " || exit /b"
    else:
        script_ext = ".sh"
        shell = ["bash"]
        build_script_contents = "# Autogenerated by LiteX\nset -e\n"
        fail_stmt = ""

    build_script_contents += """
yosys {yosys_opt} -l {build_name}.rpt {build_name}.ys{fail_stmt}
arachne-pnr {pnr_opt} -p {build_name}.pcf {build_name}.blif -o {build_name}.txt{fail_stmt}
icetime {icetime_opt} -t -p {build_name}.pcf -r {build_name}.tim {build_name}.txt{fail_stmt}
icepack {icepack_opt} {build_name}.txt {build_name}.bin{fail_stmt}
"""
    build_script_contents = build_script_contents.format(
        build_name=build_name,
        yosys_opt=yosys_opt, pnr_opt=pnr_opt, icepack_opt=icepack_opt,
        icetime_opt=icetime_opt, fail_stmt=fail_stmt)
    build_script_file = "build_" + build_name + script_ext
    tools.write_to_file(build_script_file, build_script_contents,
                        force_unix=False)
    command = shell + [build_script_file]
    r = subprocess.call(command)
    if r != 0:
        raise OSError("Subprocess failed")


class LatticeIceStormToolchain:
    attr_translate = {
        "keep": ("keep", "true"),
        "no_retiming": None,  # yosys can do retiming via the (non-default)
                              # "-retime" option to "synth_ice40", but
                              # yosys does not check for an equivalent
                              # constraint to prevent retiming on signals.
        "async_reg": None,  # yosys has no equivalent, and arachne-pnr
                            # wouldn't take advantage of it anyway.

        # While custom attributes are supported in yosys, neither
        # arachne-pnr nor icetime currently can take advantage of them
        # to add fine-grained timing constraints.
        "mr_ff": None,  # user-defined attribute
        "mr_false_path": None,  # user-defined attribute
        "ars_ff1": None,  # user-defined attribute
        "ars_ff2": None,  # user-defined attribute
        "ars_false_path": None,  # user-defined attribute

        # ice40 does not have a shift register primitive.
        "no_shreg_extract": None
    }

    special_overrides = common.icestorm_special_overrides

    def __init__(self):
        self.yosys_opt = "-q"
        self.pre_synthesis_commands = list()
        self.synthesis_commands = list()
        self.post_synthesis_commands = list()
        self.pnr_opt = "-q"
        self.icetime_opt = ""
        self.icepack_opt = ""
        self.freq_constraints = dict()

    # platform.device should be of the form "ice40-{lp384, hx1k, etc}-{tq144, etc}""
    def build(self, platform, fragment, build_dir="build", build_name="top",
              run=True):
        os.makedirs(build_dir, exist_ok=True)
        cwd = os.getcwd()
        os.chdir(build_dir)

        if not isinstance(fragment, _Fragment):
            fragment = fragment.get_fragment()
        platform.finalize(fragment)

        v_output = platform.get_verilog(fragment)
        named_sc, named_pc = platform.resolve_signals(v_output.ns)
        v_file = build_name + ".v"
        v_output.write(v_file)
        sources = platform.sources | {(v_file, "verilog", "work")}
        _build_yosys(platform.device, sources, platform.verilog_include_paths,
                     build_name, self.pre_synthesis_commands,
                     self.synthesis_commands, self.post_synthesis_commands)

        tools.write_to_file(build_name + ".pcf",
                            _build_pcf(named_sc, named_pc))
        if run:
            (family, series_size, package) = self.parse_device_string(platform.device)
            pnr_opt = self.pnr_opt + " -d " + self.get_size_string(series_size) + " -P " + package
            icetime_opt = self.icetime_opt + " -P " + package + \
                " -d " + series_size + " -c " + \
                str(max(self.freq_constraints.values(), default=0.0))
            _run_icestorm(build_name, False, self.yosys_opt, pnr_opt,
                          icetime_opt, self.icepack_opt)

        os.chdir(cwd)

        return v_output.ns

    def parse_device_string(self, device_str):
        # Arachne only understands packages based on the device size, but
        # LP for a given size supports packages that HX for the same size
        # doesn't and vice versa; we need to know the device series due to
        # icetime.
        valid_packages = {
            "lp384": ["qn32", "cm36", "cm49"],
            "lp1k": ["swg16tr", "cm36", "cm49", "cm81", "cb81", "qn84", "cm121", "cb121"],
            "hx1k": ["vq100", "cb132", "tq144"],
            "lp8k": ["cm81", "cm81:4k", "cm121", "cm121:4k", "cm225", "cm225:4k"],
            "hx8k": ["cb132", "cb132:4k", "tq144:4k", "cm225", "ct256"],
        }

        (family, series_size, package) = device_str.split("-")
        if family not in ["ice40"]:
            raise ValueError("Unknown device family")
        if series_size not in ["lp384", "lp1k", "hx1k", "lp8k", "hx8k"]:
            raise ValueError("Invalid device series/size")
        if package not in valid_packages[series_size]:
            raise ValueError("Invalid device package")
        return (family, series_size, package)

    def get_size_string(self, series_size_str):
        return series_size_str[2:]

    # icetime can only handle a single global constraint. Pending more
    # finely-tuned analysis features in arachne-pnr and IceStorm, save
    # all the constraints in a dictionary and test against the fastest clk.
    # Though imprecise, if the global design satisfies the fastest clock,
    # we can be sure all other constraints are satisfied.
    def add_period_constraint(self, platform, clk, period):
        new_freq = 1000.0/period

        if clk not in self.freq_constraints.keys():
            self.freq_constraints[clk] = new_freq
        else:
            raise ConstraintError("Period constraint already added to signal.")
