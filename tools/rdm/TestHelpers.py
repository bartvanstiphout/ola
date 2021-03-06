#!/usr/bin/python
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Library General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#
# TestHelpers.py
# Copyright (C) 2013 Peter Newman

__author__ = 'nomis52@gmail.com (Simon Newton)'


def ContainsUnprintable(s):
  """Check if a string s contain unprintable characters."""
  if type(s) == str:
    return s != s.encode('string-escape')
  elif type(s) == unicode:
    return s != s.encode('unicode-escape')
  else:
    return False;
