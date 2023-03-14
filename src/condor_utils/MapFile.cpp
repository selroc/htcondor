/***************************************************************
 *
 * Copyright (C) 1990-2007, Condor Team, Computer Sciences Department,
 * University of Wisconsin-Madison, WI.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/

#include "condor_common.h"
#include "condor_open.h"
#include "condor_debug.h"
#include "condor_config.h"
#include "HashTable.h"
#include "MapFile.h"
#include "directory.h"
#include "basename.h"

#include <unordered_map>

// THEORY OF OPERATION
//
// The in-memory representation of MapFile is a stl map of "method" to table-of-mappings
// where the table-of-mappings is a linked list of CanonicalMapEntry items, each of these
// represents one or more (consecutive) rows from the mapfile.
//
// the UserMap table is the table where method==NULL.
//
// For each table-of-mappings, the order in which we scan the list must be the order
// in which the rows were read from the file, the linked list satisfies that requirement
// and doesn't suffer from the re-allocation (copy) cost of a vector.
//
// Each list entry will either be a regex or an unordered_map, these are implemented
// as simple classes derived from the CanonicalMapEntry but NOT virtually.
// We don't want or need the cost of a vtable in this simple heirarchy. Of course
// it means that we can't do the cleanup in a class destructor, so cleanup is
// done by explicit calls to the derived classes's clear() method.
//
// All strings are stored in a string pool class (the same one the config code uses)
// and they are destroyed all at once when the MapFile class is destroyed.
// The use of a string pool reduces the allocation overhead of strings which
// results in about a 30% to 50% savings of memory vs MyString or std::string.
// It also opens up the possibility of sharing common strings, but the MapFile
// class does not currently take advantage of this.
//
// In all the V2 MapFile class uses about 2/3 of the memory that the V1 version did
// when the map is populated with regex entries.  When it is populated with simple
// literal keys, the memory usage is 1/3 of that of the V1 version.

struct hash_yourstring {
	size_t operator()(const YourString & str) const {
		return (size_t) hashFunction(str);
	}
};

typedef std::unordered_map<const YourString, const char *, hash_yourstring> LITERAL_HASH;

class CanonicalMapRegexEntry;
class CanonicalMapHashEntry;

// note: NOT virtual so that we don't have the allocation cost of a VTBL per entry
class CanonicalMapEntry {
public:
	CanonicalMapEntry * next;
	CanonicalMapEntry(char typ) : next(NULL), entry_type(typ) { memset(spare, 0, sizeof(spare)); }
	~CanonicalMapEntry();
	bool matches(const char * principal, int cch, std::vector<MyString> *groups, const char ** pcanon);
	bool is_hash_type() const { return entry_type == 2; }
protected:
	friend class MapFile;
	void dump(FILE* fp);
	char entry_type; // 0 = base, 1 = CanonicalMapRegexEntry, 2 = CanonicalMapHashEntry
	char spare[sizeof(void*)-1];
};

class CanonicalMapRegexEntry : public CanonicalMapEntry {
public:
	CanonicalMapRegexEntry() : CanonicalMapEntry(1), re_options(0), re(NULL), canonicalization(NULL) {}
	~CanonicalMapRegexEntry() { clear(); }
	void clear() { if (re) pcre2_code_free(re); re = NULL; canonicalization = NULL; }
	bool add(const char* pattern, uint32_t options, const char * canon, int * errcode, PCRE2_SIZE * erroffset);
	bool matches(const char * principal, int cch, std::vector<MyString> *groups, const char ** pcanon);
	void dump(FILE * fp) {
		fprintf(fp, "   REGEX { /<compiled_regex>/%x %s }\n", re_options, canonicalization);
	}
private:
	friend class MapFile;
	//Regex re;
	uint32_t re_options;
	pcre2_code * re;
	const char * canonicalization;
};
class CanonicalMapHashEntry : public CanonicalMapEntry {
public:
	CanonicalMapHashEntry() : CanonicalMapEntry(2), hm(NULL) {}
	~CanonicalMapHashEntry() { clear(); }
	void clear() { if (hm) { hm->clear(); delete hm; } hm = NULL; }
	bool add(const char * name, const char * canon);
	bool matches(const char * principal, int cch, std::vector<MyString> *groups, const char ** pcanon);
	static CanonicalMapHashEntry * is_type(CanonicalMapEntry * that) {
		if (that && that->is_hash_type()) { return reinterpret_cast<CanonicalMapHashEntry*>(that); }
		return NULL;
	}
	void dump(FILE * fp) {
		fprintf(fp, "   HASH {\n");
		if (hm) {
			for (auto it = hm->begin(); it != hm->end(); ++it) {
				fprintf(fp, "        \"%s\"  %s\n", it->first.c_str(), it->second);
			}
		}
		fprintf(fp, "   } # end HASH\n");
	}

private:
	friend class MapFile;
	LITERAL_HASH * hm;
};

class CanonicalMapList {
protected:
	friend class MapFile;

	CanonicalMapList() : first(NULL), last(NULL) {}
	~CanonicalMapList() {} // do NOT destroy items in the list. (they are not owned here..)

	CanonicalMapEntry * first;
	CanonicalMapEntry * last;

	// append a rule to the list for this method
	void append(CanonicalMapEntry * item) {
		ASSERT(item && (item != first) && (item != last));
		if ( ! first) { first = item; }
		else { last->next = item; }
		last = item; item->next = NULL;
	}
};

bool CanonicalMapEntry::matches(const char * principal, int cch, std::vector<MyString> *groups, const char ** pcanon)
{
	if (entry_type == 1) {
		return reinterpret_cast<CanonicalMapRegexEntry*>(this)->matches(principal, cch, groups, pcanon);
	} else if (entry_type == 2) {
		return reinterpret_cast<CanonicalMapHashEntry*>(this)->matches(principal, cch, groups, pcanon);
	}
	return false;
}

void CanonicalMapEntry::dump(FILE* fp)
{
	if (entry_type == 1) {
		reinterpret_cast<CanonicalMapRegexEntry*>(this)->dump(fp);
	} else if (entry_type == 2) {
		reinterpret_cast<CanonicalMapHashEntry*>(this)->dump(fp);
	}
}

CanonicalMapEntry::~CanonicalMapEntry() {
	if (entry_type == 1) {
		reinterpret_cast<CanonicalMapRegexEntry*>(this)->clear();
	} else if (entry_type == 2) {
		reinterpret_cast<CanonicalMapHashEntry*>(this)->clear();
	}
}

MapFile::MapFile()
{
}


MapFile::~MapFile()
{
	clear();
}

static size_t min_re_size=0, max_re_size=0, num_re=0, num_zero_re=0;
static size_t re_size(pcre2_code * re) {
	if ( !re) return 0;
	size_t cb = 0;
	uint32_t cb_uint32 = 0;
	pcre2_pattern_info(re, PCRE2_INFO_SIZE, &cb_uint32);
	cb = static_cast<size_t>(cb_uint32);
	++num_re;
	if (cb) { if (!min_re_size || (cb && (cb < min_re_size))) min_re_size = cb; max_re_size = MAX(cb, max_re_size); }
	else { ++num_zero_re; }
	return cb;
}

void get_mapfile_re_info(size_t *info) { info[0] = num_re; info[1] = num_zero_re; info[2] = min_re_size; info[3] = max_re_size; }
void clear_mapfile_re_info() { min_re_size = max_re_size =  num_re = num_zero_re = 0; }

int MapFile::size(MapFileUsage * pusage) // returns number of items in the map
{
	size_t cRegex = 0, cHash = 0, cEntries = 0, cAllocs=0, cbStructs=0;
	for (METHOD_MAP::iterator it = methods.begin(); it != methods.end(); ++it) {
		CanonicalMapList * list = it->second;
		CanonicalMapEntry * item = list->first;
		++cAllocs; cbStructs += sizeof(*list); // account for method
		while (item) {
			++cEntries; // account for list item
			if (item->is_hash_type()) {
				CanonicalMapHashEntry* hitem = reinterpret_cast<CanonicalMapHashEntry*>(item);
				++cAllocs; cbStructs += sizeof(*hitem);
				if (hitem->hm) {
					size_t chm = hitem->hm->size();
					cHash += chm;
					++cAllocs; cbStructs += sizeof(*hitem->hm);
					cAllocs += chm; cbStructs += chm*sizeof(void*)*4; // key and value are each pointers, + hash entries need a next pointer and the hash value
					cAllocs += 1; cbStructs += hitem->hm->bucket_count() * (sizeof(void*)+sizeof(size_t)); // each bucket must have an item list
				}
			} else if (item->entry_type == 1) {
				CanonicalMapRegexEntry* ritem = reinterpret_cast<CanonicalMapRegexEntry*>(item);
				++cAllocs; cbStructs += sizeof(*ritem);
				if (ritem->re) { cAllocs += 1; cbStructs += re_size(ritem->re); }  // we don't know how big a regex actually is, assuming 32 bytes
				++cRegex;
			} else {
				++cAllocs; cbStructs += sizeof(*item);
			}
			item = item->next;
		}
	}

	if (pusage) {
		memset(pusage, 0, sizeof(*pusage));
		int cb = 0, cHunks = 0, cbFree = 0;
		cb = apool.usage(cHunks, cbFree);

		pusage->cRegex = (int)cRegex;
		pusage->cHash = (int)cHash;
		pusage->cEntries = (int)cEntries;
		pusage->cMethods = (int)methods.size();
		pusage->cbStrings = cb;
		pusage->cbStructs = (int)cbStructs;
		pusage->cbWaste = cbFree;
		pusage->cAllocations = (int)(cHunks + cAllocs);
	}

	return (int)(cRegex + cHash);
}
void MapFile::reset() // remove all items, but do not free the allocation pool
{
	// all of the strings are owned by the apool which deletes them when it is destructed
	// we just have to free the CanonicalMapEntry(s)
	for (METHOD_MAP::iterator it = methods.begin(); it != methods.end(); /*advance inside the loop*/) {
		METHOD_MAP::iterator lit = it++;
		CanonicalMapList * list = lit->second;
		CanonicalMapEntry * item = list->first;
		while (item) {
			CanonicalMapEntry * prev = item;
			item = item->next;
			prev->next = NULL;
			delete prev;
		}
		methods.erase(lit);
		delete list;
	}
}
void MapFile::clear() // clear all items and free the allocation pool
{
	reset();
	apool.clear();
}

size_t
MapFile::ParseField(const std::string & line, size_t offset, std::string & field, uint32_t * popts /*=NULL*/)
{
	ASSERT(offset <= line.length());

		// We consume the leading white space
	while (offset < line.length() &&
		   (' ' == line[offset] ||
			'\t' == line[offset] ||
			'\n' == line[offset])) {
		offset++;
	}

	char chEnd = 0;
	char ch = line[offset];
	bool multiword = '"' == ch || '/' == ch;
	if (multiword) {
		chEnd = ch;
		if ( ! popts) {
			// multiword can start with / only  if popts is not null
			if (ch == '/') { chEnd = 0; multiword = false; }
		} else {
			*popts = (ch == '/') ? PCRE2_NOTEMPTY : 0;
		}
	}

		// Consume initial " (quote) or / (regex)
	if (multiword) {
		offset++;
	}

	while (offset < line.length()) {
		if (multiword) {
				// If we hit a " (quote) we are done, quotes in the
				// field are prefixed with a \ [don't end comments
				// with a backslash]
			if (chEnd == line[offset]) {
					// We consume the trailing " or /
				offset++;
				if (chEnd == '/') {
					// if this is a regex match, then it can be followed by i or U to modify the match
					while ((ch = line[offset]) != 0) {
						if (ch == 'i') {
							if (popts) {
								*popts |= PCRE2_CASELESS;
							}
						} else if (ch == 'U') {
							if (popts) {
								*popts |= PCRE2_UNGREEDY;
							}
						} else {
							break;
						}
						++offset;
					}
				}
				break;

					// If we see a \ we either write it out or if it
					// is followed by a " we strip it and output the "
					// alone
			} else if ('\\' == line[offset] && ++offset < line.length()) {
				if (chEnd == (line[offset])) {
					field += line[offset];
				} else if('\\' == (line[offset])) {
					field += '\\';
				} else {
					field += '\\';
					field += line[offset];
				}
			} else {
				field += line[offset];
			}

			offset++;
		} else {
				// This field is not multiple words, so we're done
				// when we see a space
			if (' ' == line[offset] ||
				'\t' == line[offset] ||
				'\n' == line[offset]) {
					// We don't consume the tailing white space. We
					// consume leading white space
				break;
			} else {
				field += line[offset];
			}

			offset++;
		}
	}

		// NOTE: If the field has multiple words, EOL will mark the end
		// of it, even if there is no matching " (quote)
	return offset;
}

int
MapFile::ParseCanonicalizationFile(const MyString filename, bool assume_hash /*=false*/, bool allow_include /*=true*/)
{
	FILE *file = safe_fopen_wrapper_follow(filename.c_str(), "r");
	if (NULL == file) {
		dprintf(D_ALWAYS,
				"ERROR: Could not open canonicalization file '%s' (%s)\n",
				filename.c_str(),
				strerror(errno));
		return -1;
	} else {
		dprintf(D_FULLDEBUG, "Reading mapfile %s\n", filename.c_str());
	}

	MyStringFpSource myfs(file, true);

	return ParseCanonicalization(myfs, filename.c_str(), assume_hash, allow_include);
}

int
MapFile::ParseCanonicalization(MyStringSource & src, const char * srcname, bool assume_hash /*=false*/, bool allow_include /*=true*/)
{
	int line = 0;

	while ( ! src.isEof()) {
		std::string input_line;
		size_t offset;
		std::string method;
		std::string principal;
		std::string canonicalization;

		line++;

		readLine(input_line, src); // Result ignored, we already monitor EOF

		if (input_line.empty()) {
			continue;
		}

		offset = 0;
		offset = ParseField(input_line, offset, method);

		if (method == "@include") {
			if ( ! allow_include) {
				dprintf(D_ALWAYS, "ERROR: @include directive not allowed in the map file %s (line %d)\n", srcname, line);
				continue;
			}
			std::string path;
			offset = ParseField(input_line, offset, path);
			if (path.empty()) {
				dprintf(D_ALWAYS, "ERROR: Empty filename for @include directive in the map %s (line %d)\n", srcname, line);
				continue;
			}
			if ( ! fullpath(path.c_str()) && condor_basename(srcname) > srcname) {
				MyString filen(path);
				MyString dirn; dirn.append(srcname, (int)(condor_basename(srcname) - srcname));
				dircat(dirn.c_str(), filen.c_str(), path);
			}
			StatInfo si(path.c_str());
			if (si.IsDirectory()) {
				StringList file_list;
				if ( ! get_config_dir_file_list( path.c_str(), file_list)) {
					dprintf(D_ALWAYS, "ERROR: Could not include dir %s\n", path.c_str());
					continue;
				}

				file_list.rewind();
				char const *fname;
				while ((fname = file_list.next())) {
					// read file, but don't allow it to have @include directives
					ParseCanonicalizationFile(fname, assume_hash, false);
				}
			} else {
				// read file, but don't allow it to have @include directives
				ParseCanonicalizationFile(path, assume_hash, false);
			}
			continue;
		}

		if (method.length() == 0 || method[0] == '#') continue; // ignore blank and comment lines
		uint32_t regex_opts = assume_hash ? 0 : PCRE2_NOTEMPTY;
		offset = ParseField(input_line, offset, principal, assume_hash ? &regex_opts : NULL);
		offset = ParseField(input_line, offset, canonicalization);

		if (method.empty() ||
			principal.empty() ||
			canonicalization.empty()) {
				dprintf(D_ALWAYS, "ERROR: Error parsing line %d of %s.  (Method=%s) (Principal=%s) (Canon=%s) Skipping to next line.\n",
						line, srcname, method.c_str(), principal.c_str(), canonicalization.c_str());

				continue;
		}

		dprintf(D_FULLDEBUG,
				"MapFile: Canonicalization File: method='%s' principal='%s' canonicalization='%s'\n",
				method.c_str(),
				principal.c_str(),
				canonicalization.c_str());

/*
		Regex *re = new Regex;
		if (NULL == re) {
			dprintf(D_ALWAYS, "ERROR: Failed to allocate Regex!\n");
		}
*/
		CanonicalMapList* list = GetMapList(method.c_str());
		ASSERT(list);
		AddEntry(list, regex_opts, principal.c_str(), canonicalization.c_str());
	}

	return 0;
}

int
MapFile::ParseUsermapFile(const MyString filename, bool assume_hash /*=false*/)
{
	FILE *file = safe_fopen_wrapper_follow(filename.c_str(), "r");
	if (NULL == file) {
		dprintf(D_ALWAYS,
				"ERROR: Could not open usermap file '%s' (%s)\n",
				filename.c_str(),
				strerror(errno));
		return -1;
	}

	MyStringFpSource myfs(file, true);

	return ParseUsermap(myfs, filename.c_str(), assume_hash);
}

int
MapFile::ParseUsermap(MyStringSource & src, const char * srcname, bool assume_hash /*=false*/)
{
	int line = 0;

    while ( ! src.isEof()) {
		std::string input_line;
		size_t offset;
		std::string canonicalization;
		std::string user;

		line++;

		readLine(input_line, src); // Result ignored, we already monitor EOF

		if (input_line.empty()) {
			continue;
		}

		offset = 0;
		uint32_t regex_opts = assume_hash ? 0 : PCRE2_NOTEMPTY;
		offset = ParseField(input_line, offset, canonicalization, assume_hash ? &regex_opts : NULL);
		if (canonicalization.length() == 0 || canonicalization[0] == '#') continue; // ignore blank and comment lines
		offset = ParseField(input_line, offset, user);

		dprintf(D_FULLDEBUG,
				"MapFile: Usermap File: canonicalization='%s' user='%s'\n",
				canonicalization.c_str(),
				user.c_str());

		if (canonicalization.empty() ||
			user.empty()) {
				dprintf(D_ALWAYS, "ERROR: Error parsing line %d of %s.\n",
						line, srcname);
				return line;
		}
	
		CanonicalMapList* list = GetMapList(NULL); // NULL is the 'method' key for the usermap list
		ASSERT(list);
		AddEntry(list, regex_opts, canonicalization.c_str(), user.c_str());
	}

	return 0;
}


void MapFile::dump(FILE* fp)
{
	for (auto it = methods.begin(); it != methods.end(); ++it) {
		const char * method = it->first.c_str();
		fprintf(fp, "\n%s = {\n", method);

		for (CanonicalMapEntry * entry = it->second->first; entry; entry = entry->next) {
			entry->dump(fp);
		}
		fprintf(fp, "} # end %s\n", method);
	}
}

int
MapFile::GetCanonicalization(const MyString& method,
							 const MyString& principal,
							 MyString & canonicalization)
{
	bool match_found = false;

	const char * pcanon;
	std::vector<MyString> groups;

	METHOD_MAP::iterator found = methods.find(method.c_str());
	if (found != methods.end() && found->second) {
		match_found = FindMapping(found->second, principal, &groups, &pcanon);
		if (match_found) {
			PerformSubstitution(groups, pcanon, canonicalization);
		}
	}
	return match_found ? 0 : -1;
}

// find or create a CanonicalMapList for the given method.
// use NULL as the method value for for the usermap file
CanonicalMapList* MapFile::GetMapList(const char * method) // method is NULL for User table
{
	CanonicalMapList* list = NULL;

	METHOD_MAP::iterator it = methods.find(method);
	if (it != methods.end()) {
		list = it->second;
	} else {
		YourString key(method ? apool.insert(method) : NULL);
		std::pair<METHOD_MAP::iterator, bool> pp = methods.insert(std::pair<YourString, CanonicalMapList*>(key, (CanonicalMapList *)NULL));
		if (pp.second) {
			// insert succeeded
			// it = pp.first;
			list = new CanonicalMapList;
			methods[key] = list;
		}
	}

	return list;
}


void MapFile::AddEntry(CanonicalMapList* list, uint32_t regex_opts, const char * principal, const char * canonicalization)
{
	//PRAGMA_REMIND("stringspace these??")
	const char * canon = apool.insert(canonicalization);

	if (regex_opts) {
		regex_opts &= ~PCRE2_NOTEMPTY; // we use this as a trigger, don't pass it down.
		CanonicalMapRegexEntry * rxme = new CanonicalMapRegexEntry;
		int errcode; PCRE2_SIZE erroffset;
		if ( ! rxme->add(principal, regex_opts, canon, &errcode, &erroffset)) {
			dprintf(D_ALWAYS, "ERROR: Error compiling expression '%s' at offset %zu -- PCRE2 error code %d.  this entry will be ignored.\n", principal, erroffset, errcode);
			delete rxme;
		} else {
			list->append(rxme);
		}
	} else {
		// if the previous entry was a hash type entry, then we will just add an item to that hash
		CanonicalMapHashEntry * hme = CanonicalMapHashEntry::is_type(list->last);
		if ( ! hme) {
			// if it was not, allocate a new hash type entry and add it to the map list
			hme = new CanonicalMapHashEntry();
			list->append(hme);
		}
		// add the item to the hash
		hme->add(apool.insert(principal), canon);
	}
}


bool CanonicalMapHashEntry::matches(const char * principal, int /*cch*/, std::vector<MyString> *groups, const char ** pcanon)
{
	LITERAL_HASH::iterator found = hm->find(principal);
	if (found != hm->end()) {
		if (pcanon) *pcanon = found->second;
		if (groups) {
			groups->clear();
			groups->push_back(found->first.ptr());
		}
		return true;
	}
	return false;
}

bool CanonicalMapRegexEntry::matches(const char * principal, int cch, std::vector<MyString> *groups, const char ** pcanon)
{
	pcre2_match_data * matchdata = pcre2_match_data_create_from_pattern(re, NULL);
	PCRE2_SPTR principal_pcre2str = reinterpret_cast<const unsigned char *>(principal);

	int rc = pcre2_match(re, principal_pcre2str, static_cast<PCRE2_SIZE>(cch),
						0, // Index in string from which to start matching
						re_options,
						matchdata, NULL);
	if (rc <= 0) {
		// does not match
		pcre2_match_data_free(matchdata);
		return false;
	}

	if (pcanon) *pcanon = this->canonicalization;
	if (groups) {
		groups->clear();
		PCRE2_SIZE * ovector = pcre2_get_ovector_pointer(matchdata);
		for (int i = 0; i < rc; i++) {
			int ix1 = static_cast<int>(ovector[i * 2]);
			int ix2 = static_cast<int>(ovector[i * 2 + 1]);
			groups->push_back({});
			(*groups)[i].set(&principal[ix1], ix2 - ix1);
		}
	}

	pcre2_match_data_free(matchdata);

	return true;
}


bool CanonicalMapRegexEntry::add(const char * pattern, uint32_t options, const char * canon, int * errcode, PCRE2_SIZE * erroffset)
{
	if (re) pcre2_code_free(re);
	PCRE2_SPTR pattern_pcre2str = reinterpret_cast<const unsigned char *>(pattern);
	re = pcre2_compile(pattern_pcre2str, PCRE2_ZERO_TERMINATED, options, errcode, erroffset, NULL);
	if (re) {
		canonicalization = canon;
		return true;
	}
	return false;
}

bool CanonicalMapHashEntry::add(const char * name, const char * canon)
{
	if ( ! hm) hm = new LITERAL_HASH;
	if (hm->find(name) == hm->end()) {
		(*hm)[name] = canon;
		return true;
	}
	return false;
}

int
MapFile::GetUser(const MyString canonicalization,
				 MyString & user)
{
	bool match_found = false;

	const char * pcanon;
	std::vector<MyString> groups;

	METHOD_MAP::iterator found = methods.find(NULL);
	if (found != methods.end() && found->second) {
		match_found = FindMapping(found->second, canonicalization, &groups, &pcanon);
		if (match_found) {
			PerformSubstitution(groups, pcanon, user);
		}
	}
	return match_found ? 0 : -1;
}


bool
MapFile::FindMapping(CanonicalMapList* list,       // in: the mapping data set
					const MyString & input,         // in: the input to be matched and mapped.
					std::vector<MyString> * groups,  // out: match groups from the input
					const char ** pcanon)         // out: canonicalization pattern
{
	for (CanonicalMapEntry * entry = list->first; entry; entry = entry->next) {
		if (entry->matches(input.c_str(), input.length(), groups, pcanon)) {
			return true;
		}
	}
	return false;
}

void
MapFile::PerformSubstitution(std::vector<MyString> & groups,
							 const char * pattern,
							 MyString & output)
{
	for (int index = 0; pattern[index]; index++) {
		if ('\\' == pattern[index]) {
			index++;
			if (pattern[index]) {
				if ('0' <= pattern[index] &&
					'9' >= pattern[index]) {
					size_t match = (size_t) pattern[index] - '0';
					if (groups.size() > match) {
						output += groups[match];
						continue;
					}
				}

				output += '\\';
			}
		}

		output += pattern[index];
	}
}
