/*
 * ReadYourWrites.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FDBCLIENT_READYOURWRITES_H
#define FDBCLIENT_READYOURWRITES_H
#pragma once

#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/KeyRangeMap.h"
#include "fdbclient/RYWIterator.h"
#include "fdbclient/ISingleThreadTransaction.h"
#include <list>

// SOMEDAY: Optimize getKey to avoid using getRange

struct ReadYourWritesTransactionOptions {
	bool readYourWritesDisabled : 1;
	bool readAheadDisabled : 1;
	bool readSystemKeys : 1;
	bool writeSystemKeys : 1;
	bool nextWriteDisableConflictRange : 1;
	bool debugRetryLogging : 1;
	bool disableUsedDuringCommitProtection : 1;
	bool specialKeySpaceRelaxed : 1;
	bool specialKeySpaceChangeConfiguration : 1;
	double timeoutInSeconds;
	int maxRetries;
	int snapshotRywEnabled;
	bool bypassUnreadable : 1;

	ReadYourWritesTransactionOptions() {}
	explicit ReadYourWritesTransactionOptions(Transaction const& tr);
	void reset(Transaction const& tr);
	bool getAndResetWriteConflictDisabled();
};

struct TransactionDebugInfo : public ReferenceCounted<TransactionDebugInfo> {
	std::string transactionName;
	double lastRetryLogTime;

	TransactionDebugInfo() : transactionName(""), lastRetryLogTime() {}
};

// Values returned by a ReadYourWritesTransaction will contain a reference to the transaction's arena. Therefore,
// keeping a reference to a value longer than its creating transaction would hold all of the memory generated by the
// transaction
// If options.readYourWritesDisabled, rely on NativeAPI to handle everything. Otherwise, read NativeAPI with
// Snapshot::True and handle read conflicts at ReadYourWritesTransaction, write NativeAPI with AddConflictRange::False
// and handle write conflicts at ReadYourWritesTransaction, eventually send this information to NativeAPI on commit.
class ReadYourWritesTransaction final : NonCopyable,
                                        public ISingleThreadTransaction,
                                        public FastAllocated<ReadYourWritesTransaction> {
public:
	explicit ReadYourWritesTransaction(Database const& cx, Optional<TenantName> tenant = Optional<TenantName>());
	~ReadYourWritesTransaction();

	void construct(Database const&) override;
	void construct(Database const&, TenantName const& tenant) override;
	void setVersion(Version v) override { tr.setVersion(v); }
	Future<Version> getReadVersion() override;
	Optional<Version> getCachedReadVersion() const override { return tr.getCachedReadVersion(); }
	Future<Optional<Value>> get(const Key& key, Snapshot = Snapshot::False) override;
	Future<Key> getKey(const KeySelector& key, Snapshot = Snapshot::False) override;
	Future<RangeResult> getRange(const KeySelector& begin,
	                             const KeySelector& end,
	                             int limit,
	                             Snapshot = Snapshot::False,
	                             Reverse = Reverse::False) override;
	Future<RangeResult> getRange(KeySelector begin,
	                             KeySelector end,
	                             GetRangeLimits limits,
	                             Snapshot = Snapshot::False,
	                             Reverse = Reverse::False) override;
	Future<RangeResult> getRange(const KeyRange& keys,
	                             int limit,
	                             Snapshot snapshot = Snapshot::False,
	                             Reverse reverse = Reverse::False) {
		return getRange(KeySelector(firstGreaterOrEqual(keys.begin), keys.arena()),
		                KeySelector(firstGreaterOrEqual(keys.end), keys.arena()),
		                limit,
		                snapshot,
		                reverse);
	}
	Future<RangeResult> getRange(const KeyRange& keys,
	                             GetRangeLimits limits,
	                             Snapshot snapshot = Snapshot::False,
	                             Reverse reverse = Reverse::False) {
		return getRange(KeySelector(firstGreaterOrEqual(keys.begin), keys.arena()),
		                KeySelector(firstGreaterOrEqual(keys.end), keys.arena()),
		                limits,
		                snapshot,
		                reverse);
	}
	Future<MappedRangeResult> getMappedRange(KeySelector begin,
	                                         KeySelector end,
	                                         Key mapper,
	                                         GetRangeLimits limits,
	                                         Snapshot = Snapshot::False,
	                                         Reverse = Reverse::False) override;

	[[nodiscard]] Future<Standalone<VectorRef<const char*>>> getAddressesForKey(const Key& key) override;
	Future<Standalone<VectorRef<KeyRef>>> getRangeSplitPoints(const KeyRange& range, int64_t chunkSize) override;
	Future<int64_t> getEstimatedRangeSizeBytes(const KeyRange& keys) override;

	Future<Standalone<VectorRef<KeyRangeRef>>> getBlobGranuleRanges(const KeyRange& range) override;
	Future<Standalone<VectorRef<BlobGranuleChunkRef>>> readBlobGranules(const KeyRange& range,
	                                                                    Version begin,
	                                                                    Optional<Version> readVersion,
	                                                                    Version* readVersionOut) override;

	void addReadConflictRange(KeyRangeRef const& keys) override;
	void makeSelfConflicting() override { tr.makeSelfConflicting(); }

	void atomicOp(const KeyRef& key, const ValueRef& operand, uint32_t operationType) override;
	void set(const KeyRef& key, const ValueRef& value) override;
	void clear(const KeyRangeRef& range) override;
	void clear(const KeyRef& key) override;

	[[nodiscard]] Future<Void> watch(const Key& key) override;

	void addWriteConflictRange(KeyRangeRef const& keys) override;

	[[nodiscard]] Future<Void> commit() override;
	Version getCommittedVersion() const override { return tr.getCommittedVersion(); }
	int64_t getApproximateSize() const override { return approximateSize; }
	[[nodiscard]] Future<Standalone<StringRef>> getVersionstamp() override;

	void setOption(FDBTransactionOptions::Option option, Optional<StringRef> value = Optional<StringRef>()) override;

	[[nodiscard]] Future<Void> onError(Error const& e) override;

	// These are to permit use as state variables in actors:
	ReadYourWritesTransaction() : cache(&arena), writes(&arena) {}
	void operator=(ReadYourWritesTransaction&& r) noexcept;
	ReadYourWritesTransaction(ReadYourWritesTransaction&& r) noexcept;

	void cancel() override;
	void reset() override;
	void debugTransaction(UID dID) override { tr.debugTransaction(dID); }

	Future<Void> debug_onIdle() { return reading; }

	// Wait for all reads that are currently pending to complete
	Future<Void> pendingReads() { return resetPromise.getFuture() || reading; }
	// Throws before the lifetime of this transaction ends
	Future<Void> resetFuture() { return resetPromise.getFuture(); }

	void checkDeferredError() const override {
		tr.checkDeferredError();
		if (deferredError.code() != invalid_error_code)
			throw deferredError;
	}

	void getWriteConflicts(KeyRangeMap<bool>* result) override;

	Database getDatabase() const { return tr.getDatabase(); }

	Reference<const TransactionState> getTransactionState() const { return tr.trState; }

	void setTransactionID(uint64_t id);
	void setToken(uint64_t token);

	// Read from the special key space readConflictRangeKeysRange
	RangeResult getReadConflictRangeIntersecting(KeyRangeRef kr);
	// Read from the special key space writeConflictRangeKeysRange
	RangeResult getWriteConflictRangeIntersecting(KeyRangeRef kr);

	bool specialKeySpaceRelaxed() const { return options.specialKeySpaceRelaxed; }
	bool specialKeySpaceChangeConfiguration() const { return options.specialKeySpaceChangeConfiguration; }

	KeyRangeMap<std::pair<bool, Optional<Value>>>& getSpecialKeySpaceWriteMap() { return specialKeySpaceWriteMap; }
	bool readYourWritesDisabled() const { return options.readYourWritesDisabled; }
	const Optional<std::string>& getSpecialKeySpaceErrorMsg() { return specialKeySpaceErrorMsg; }
	void setSpecialKeySpaceErrorMsg(const std::string& msg) { specialKeySpaceErrorMsg = msg; }
	Transaction& getTransaction() { return tr; }

	Optional<TenantName> getTenant() { return tr.getTenant(); }

	// used in template functions as returned Future type
	template <typename Type>
	using FutureT = Future<Type>;

private:
	friend class RYWImpl;

	Arena arena;
	Transaction tr;
	SnapshotCache cache;
	WriteMap writes;
	CoalescedKeyRefRangeMap<bool> readConflicts;
	Map<Key, std::vector<Reference<Watch>>> watchMap; // Keys that are being watched in this transaction
	Promise<Void> resetPromise;
	AndFuture reading;
	int retries;
	int64_t approximateSize;
	Future<Void> timeoutActor;
	double creationTime;
	bool commitStarted;

	// For reading conflict ranges from the special key space
	VectorRef<KeyRef> versionStampKeys;
	Future<Standalone<StringRef>> versionStampFuture;
	Standalone<VectorRef<KeyRangeRef>>
	    nativeReadRanges; // Used to read conflict ranges after committing an ryw disabled transaction
	Standalone<VectorRef<KeyRangeRef>>
	    nativeWriteRanges; // Used to read conflict ranges after committing an ryw disabled transaction

	Reference<TransactionDebugInfo> transactionDebugInfo;

	KeyRangeMap<std::pair<bool, Optional<Value>>> specialKeySpaceWriteMap;
	Optional<std::string> specialKeySpaceErrorMsg;

	void resetTimeout();
	void updateConflictMap(KeyRef const& key, WriteMap::iterator& it); // pre: it.segmentContains(key)
	void updateConflictMap(
	    KeyRangeRef const& keys,
	    WriteMap::iterator& it); // pre: it.segmentContains(keys.begin), keys are already inside this->arena
	void writeRangeToNativeTransaction(KeyRangeRef const& keys);

	void resetRyow(); // doesn't reset the encapsulated transaction, or creation time/retry state
	KeyRef getMaxReadKey();
	KeyRef getMaxWriteKey();

	bool checkUsedDuringCommit();

	void debugLogRetries(Optional<Error> error = Optional<Error>());

	void setOptionImpl(FDBTransactionOptions::Option option, Optional<StringRef> value = Optional<StringRef>());
	void applyPersistentOptions();

	std::vector<std::pair<FDBTransactionOptions::Option, Optional<Standalone<StringRef>>>> persistentOptions;
	ReadYourWritesTransactionOptions options;
};

#endif
