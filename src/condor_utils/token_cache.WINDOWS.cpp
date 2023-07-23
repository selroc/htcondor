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
#include "token_cache.h"

token_cache::token_cache() : current_age(1), dummy(0) {
}

token_cache::~token_cache() {
	for (auto& [index, ent]: TokenTable) {
		delete ent;
	}
}

/* returns cached user handle if we have it, otherwise NULL if we don't */
HANDLE
token_cache::getToken(const char* username, const char* domain_raw) {

	char* domain = strdup(domain_raw);
	strupr(domain); // force all domain names to be upper case

	token_cache_entry *entry;
	std::string key(username);
	key += "@";
	key += domain;

	// now domain has been concatenated, so we can dump it
	free(domain);
	domain = NULL;

	auto itr = TokenTable.find(key);
	if (itr == TokenTable.end()) {
		// couldn't find it
		return NULL;
	} else {
		return itr->second->user_token;
	}
}

/* stores the given token in our cache and returns true on success. */
bool 
token_cache::storeToken(const char* username, const char* domain_raw,
					   	HANDLE token) {

	char* domain = strdup(domain_raw);
	strupr(domain); // force all domain names to be upper case
	
	if ( getToken(username, domain) ) {
		// if we already have it, just return.
		free(domain);
		return true;
	}

	token_cache_entry *entry;
	std::string key(username);
	key += "@";
	key += domain;

	// now domain has been concatenated, so we can dump it
	free(domain);
	domain = NULL;

	entry = new(token_cache_entry);
	entry->user_token = token;
	entry->age = getNextAge();

	if ( getCacheSize() >= MAX_CACHE_SIZE ) {
		// we need to overwrite a cache entry, since the 
		// max cache size has been reached.

		dprintf(D_FULLDEBUG, "token_cache: Removing oldest token to make space.\n");
		removeOldestToken();
	}

	auto [itr, worked] = TokenTable.insert({key, entry});
	if (worked == false) {
		dprintf(D_ALWAYS, "token_cache: failed to cache token!\n");
		return false;
	}
	return true;
}

void
token_cache::removeOldestToken() {

	token_cache_entry *oldest_ent = NULL;
	std::string oldest_index;
	int oldest_age;

	// We want to start with the "youngest" so we don't skip anybody
	oldest_age = current_age; 

	for (auto& [index, ent]: TokenTable) {
		if ( isOlder(ent->age, oldest_age) ) {
			oldest_index = index;
			oldest_ent = ent;
			oldest_age = ent->age;
		}
	}
	
	if ( oldest_ent ) { // we may not remove anything if cache is empty
		CloseHandle ( oldest_ent->user_token );
		delete oldest_ent;
		TokenTable.erase(oldest_index);
	}
}

/* return the contents of the cache in the form of a string.
 *
 * nice for debugging. */
std::string
token_cache::cacheToString() {
	std::string cache_string;

	for (auto& [index, ent]: TokenTable) {
		cache_string += index + "\n";	
	}
	return cache_string;
}
