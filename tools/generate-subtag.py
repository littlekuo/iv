#!/usr/bin/python
# -*- coding: utf-8 -*-

import sys
import string
import re
import bisect

HEADER = """
// This file is auto-generated header by tools/generate-subtag.py.
// So do not edit this file.
#ifndef IV_I18N_LANGUAGE_TAG_H_
#define IV_I18N_LANGUAGE_TAG_H_
#include <iv/detail/unordered_map.h>
#include <iv/detail/array.h>
#include <iv/stringpiece.h>
namespace iv {
namespace core {
namespace i18n {
namespace i18n_detail {
}  // namespace i18n_detail

// Irregular / regular grandfathered language tags and Preferred-Value.
// Following tags don't provide Preferred-Value in registry.
// So we define fallback tags by executing ICU canonicalizations.
//     cel-graulish cel-graulis
//     en-GB-oed    en-GB-x-oed
//     i-default    en-x-i-default
//     i-enochian   x-i-enochian
//     i-mingo      see-x-i-mingo
//     zh-min       nan-x-zh-min
typedef std::array<std::pair<StringPiece, StringPiece>, %d> GrandfatheredArray;
static const GrandfatheredArray kGrandfathered = { {
%s
} };

// Redundant tags are replaced to Preferred-Value.
// If tag don't provide Preferred-Value, we don't replace it.
typedef std::array<std::pair<StringPiece, StringPiece>, %d> RedundantArray;
static const RedundantArray kRedundant = { {
%s
} };

typedef std::unordered_map<std::string, std::string> TagMap;

inline const TagMap& Grandfathered() {
  static const TagMap map(kGrandfathered.begin(), kGrandfathered.end());
  return map;
}

inline const TagMap& Redundant() {
  static const TagMap map(kRedundant.begin(), kRedundant.end());
  return map;
}

} } }  // namespace iv::core::i18n
#endif  // IV_I18N_LANGUAGE_TAG_H_
"""

LOWER = re.compile('^[a-z0-9]+$')
UPPER = re.compile('^[A-Z0-9]+$')
TITLE = re.compile('^[A-Z0-9][a-z0-9]*$')

def is_lower(str):
  return LOWER.match(str)

def is_upper(str):
  return UPPER.match(str)

def is_title(str):
  return TITLE.match(str)

class DB(object):
  PATTERN = re.compile('^(?P<key>.+)\s*:\s*(?P<value>.+)$')
  RANGE = re.compile('(?P<first>[^\.]+)\.\.(?P<last>[^\.]+)')

  def __init__(self, source):
    self.__registry = []
    item = {}
    prev_key = None
    with open(source) as c:
      for line in c:
        line = line.strip()
        if line == '%%':
          # delimiter
          self.validate_and_append(item)
          item = {}
          prev_key = None
        else:
          m = self.PATTERN.match(line)
          if m:
            key = m.group('key')
            value = m.group('value')
            if item.has_key(key):
              prev = item[key]
              if isinstance(prev, list):
                item[key].append(value)
              else:
                item[key] = [item[key], value]
            else:
              item[key] = value
            prev_key = key
          else:
            if prev_key:
              v = item[prev_key]
              if isinstance(v, list):
                v[-1] = v[-1] + ' ' + line
              else:
                item[prev_key] = item[prev_key] + ' ' + line
      self.validate_and_append(item)
      self.extract_registry()

  def registry(self):
    return self.__registry

  def validate_and_append(self, item):
    if item.has_key('Type'):
      self.__registry.append(item)

  def extract_registry(self):
    pass
#    for item in self.__registry:
#      sub = item['Subtag']
#      m = self.RANGE.match(sub)
#      if m:
#        for tag in range(m.group('first'), m.group('last')):
#          print tag

def main(source):
  db = DB(source)

  grandfathered = [
      '  std::make_pair("cel-graulish", "cel-graulis")',
      '  std::make_pair("en-gb-oed", "en-GB-x-oed")',
      '  std::make_pair("i-default", "en-x-i-default")',
      '  std::make_pair("i-enochian", "x-i-enochian")',
      '  std::make_pair("i-mingo", "see-x-i-mingo")',
      '  std::make_pair("zh-min", "nan-x-zh-min")',
  ]
  for item in filter(lambda i: i['Type'] == 'grandfathered', db.registry()):
    if item.has_key('Preferred-Value'):
      grandfathered.append(
          '  std::make_pair("%s", "%s")' % (item['Tag'].lower(), item['Preferred-Value']))

  redundant = []
  for item in filter(lambda i: i['Type'] == 'redundant', db.registry()):
    if item.has_key('Preferred-Value'):
      redundant.append(
          '  std::make_pair("%s", "%s")' % (item['Tag'].lower(), item['Preferred-Value']))

  # all language tag should be title case
  for item in filter(lambda i: i['Type'] == 'language', db.registry()):
    if not is_lower(item['Subtag']):
      #print item
      pass

  # all extlang tag should be title case
  for item in filter(lambda i: i['Type'] == 'extlang', db.registry()):
    assert is_lower(item['Subtag']), item['Subtag']

  # all script tag should be title case
  for item in filter(lambda i: i['Type'] == 'script', db.registry()):
    if not is_title(item['Subtag']):
      #print item
      pass

  # all variant tag should be title case
  for item in filter(lambda i: i['Type'] == 'variant', db.registry()):
    assert is_lower(item['Subtag']), item['Subtag']

  print (HEADER % (len(grandfathered), ',\n'.join(grandfathered), len(redundant), ',\n'.join(redundant))).strip()

if __name__ == '__main__':
  main(sys.argv[1])
