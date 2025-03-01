# This file is part of VoltDB.

# Copyright (C) 2008-2016 VoltDB Inc.
#
# This file contains original code and/or modifications of original code.
# Any modifications made by VoltDB Inc. are licensed under the following
# terms and conditions:
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be
# included in all copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

# This script assumes a relative location in a root subdirectory of a voltdb
# distribution. The logic is intentionally minimal since almost all of the
# heavy lifting happens in runner.main(). The script name determines the verbs
# that are loaded from <name>.d subdirectories. It loads the version number
# from version.txt in the script's parent directory. It can be copied to other
# names, and also to other locations if the path-building is adjusted. The
# description should also be changed if re-used for another base command.
import sys
import os
from optparse import OptionParser
from os.path import expanduser


try:
    # ensure version 2.6+ of python
    if sys.version_info[0] == 2 and sys.version_info[1] < 6:
        for dir in os.environ['PATH'].split(':'):
            for name in ('python2.7', 'python2.6'):
                path = os.path.join(dir, name)
                if os.path.exists(path):
                    print 'Re-running with %s...' % path
                    os.execv(path, [path] + sys.argv)
        sys.stderr.write("This script requires Python 2.6 or newer. Please install " +
                         "a more recent Python release and retry.\n")
        sys.exit(-1)

    cmd_dir, cmd_name = os.path.split(os.path.realpath(sys.argv[0]))
    # Adjust these variables as needed for other base commands, locations, etc..
    base_dir = os.path.realpath(os.path.join(cmd_dir,'../../../'))
    version = open(os.path.join(base_dir, 'version.txt')).read().strip()
    description = 'Command line interface to VoltDB functions.'
    standalone  = False
    # Tweak the Python library path to call voltcli.runner.main().
    # Possible installed library locations.
    if os.path.isdir('/opt/lib/voltdb/python'):
        sys.path.insert(0, '/opt/lib/voltdb/python')
    if os.path.isdir('/usr/share/lib/voltdb/python'):
        sys.path.insert(0, '/usr/share/lib/voltdb/python')
    if os.path.isdir('/usr/lib/voltdb/python'):
        sys.path.insert(0, '/usr/lib/voltdb/python')
    # Library location relative to script.
    sys.path.insert(0, os.path.join(base_dir, 'lib', 'python'))
    sys.path.insert(0, os.path.join(base_dir, 'lib/python', 'vdm'))
    from voltcli import runner
    from server import HTTPListener

# Be selective about exceptions to avoid masking load-time library exceptions.
except (IOError, OSError, ImportError), e:
    sys.stderr.write('Exception (%s): %s\n' % (e.__class__.__name__, str(e)))
    sys.exit(1)


def main():
    parser = OptionParser(usage="usage: %prog [options] filepath",
                          version="%prog 1.0")
    parser.add_option("-p", "--path",
                  action="store", type="string", dest="filepath")
    parser.add_option("-s", "--server",
                  action="store", type="string", dest="server")
    (options, args) = parser.parse_args()

    arr = [{
        "filepath": options.filepath,
        "server": options.server
    }]

    return arr


if __name__ == '__main__':
  options = main()

  path = options[0]['filepath']
  server = options[0]['server']

app_root = os.path.dirname(os.path.abspath(__file__))
os.chdir(os.path.normpath(app_root))

if path is None:
    home = expanduser("~")
    # path = home + '.vdm' if home.endswith('/') else home + '/' + '.vdm'
    path = os.path.join(home, '.vdm')

if os.path.isdir(str(path)):
    if os.access(str(path), os.W_OK):
        HTTPListener.main(runner, HTTPListener, path, server)
    else:
        sys.stderr.write('Error: There is no permission to create file in this folder. '
                         'Unable to start VDM.')
        sys.exit(1)
else:
    try:
        os.makedirs(path)
        HTTPListener.main(runner, HTTPListener, path, server)
    except Exception, err:
        sys.stderr.write('Exception (%s): %s\n' % (err.__class__.__name__, str(err)))
        sys.exit(1)
