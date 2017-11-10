import os
import subprocess

from litex.build.generic_programmer import GenericProgrammer
from litex.build import tools


class LatticeProgrammer(GenericProgrammer):
    needs_bitreverse = False

    def __init__(self, xcf_template):
        self.xcf_template = xcf_template

    def load_bitstream(self, bitstream_file, toolchain_path=''):
        xcf_file = bitstream_file.replace(".bit", ".xcf")
        xcf_content = self.xcf_template.format(bitstream_file=bitstream_file)
        tools.write_to_file(xcf_file, xcf_content)
        if toolchain_path:
            pgrcmd = os.path.join(toolchain_path, 'bin/lin64/pgrcmd')
        else:
            pgrcmr = 'pgrcmr'
        subprocess.call([pgrcmd, "-infile", xcf_file])


class IceStormProgrammer(GenericProgrammer):
    needs_bitreverse = False

    def flash(self, address, bitstream_file):
        subprocess.call(["iceprog", "-o", str(address), bitstream_file])

    def load_bitstream(self, bitstream_file):
        subprocess.call(["iceprog", "-S", bitstream_file])


class IceBurnProgrammer(GenericProgrammer):
    def __init__(self, iceburn_path):
        GenericProgrammer.__init__(self)
        self.iceburn = iceburn_path

    needs_bitreverse = False

    def load_bitstream(self, bitstream_file):
        subprocess.call([self.iceburn, "-evw", bitstream_file])


class TinyFpgaBProgrammer(GenericProgrammer):
    needs_bitreverse = False

    # The default flash address you probably want is 0x30000; the image at
    # address 0 is for the bootloader.
    def flash(self, address, bitstream_file):
        subprocess.call(["tinyfpgab", "-a", str(address), "-p",
                        bitstream_file])
