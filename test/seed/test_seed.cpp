#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../../src/seed/seed.h"

TEST(SeedTest, Sha256MatchesTheStandardVectors) {
	EXPECT_EQ(seed::sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	EXPECT_EQ(seed::sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	EXPECT_EQ(seed::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
			  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

	// A message on a block boundary and one forcing a second padding block.
	EXPECT_EQ(seed::sha256_hex(std::string(55, 'a')),
			  "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
	EXPECT_EQ(seed::sha256_hex(std::string(56, 'a')),
			  "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
	EXPECT_EQ(seed::sha256_hex(std::string(64, 'a')),
			  "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

TEST(SeedTest, DeriveMatchesTheGoImplementation) {
	// Computed from the Go implementation's internal/seed, so runs compare across both engines.
	EXPECT_EQ(seed::derive(1, ""), 0x7e36c268efa59d9dULL);
	EXPECT_EQ(seed::derive(1, "shuffle/0/0/0"), 0xbff22bb7ff83f79fULL);
	EXPECT_EQ(seed::derive(42, "network-loss/5/1/2/3"), 0x099a2e65d4b66d53ULL);
	EXPECT_EQ(seed::derive(0, "a"), 0x2c0f0c458addd0a2ULL);
}

TEST(SeedTest, DeriveSeparatesSeedsAndLabels) {
	EXPECT_NE(seed::derive(1, "shuffle/0/0/0"), seed::derive(2, "shuffle/0/0/0"));
	EXPECT_NE(seed::derive(1, "shuffle/0/0/0"), seed::derive(1, "shuffle/0/0/1"));
	EXPECT_EQ(seed::derive(1, "shuffle/0/0/0"), seed::derive(1, "shuffle/0/0/0"));
}

TEST(SeedTest, SplitMix64MatchesTheReferenceStream) {
	seed::SplitMix64 stream(1);
	EXPECT_EQ(stream.next(), 0x910a2dec89025cc1ULL);
	EXPECT_EQ(stream.next(), 0xbeeb8da1658eec67ULL);
	EXPECT_EQ(stream.next(), 0xf893a2eefb32555eULL);
}

TEST(SeedTest, ShuffleIsReproducibleAndDependsOnTheLabel) {
	const std::vector<int> original = {0, 1, 2, 3, 4, 5, 6, 7};

	std::vector<int> first  = original;
	std::vector<int> second = original;
	seed::shuffle(first, 99, "shuffle/1/0/2");
	seed::shuffle(second, 99, "shuffle/1/0/2");
	EXPECT_EQ(first, second);

	std::vector<int> different_label = original;
	seed::shuffle(different_label, 99, "shuffle/1/0/3");
	EXPECT_NE(first, different_label);

	std::vector<int> different_seed = original;
	seed::shuffle(different_seed, 100, "shuffle/1/0/2");
	EXPECT_NE(first, different_seed);

	// A shuffle is a permutation, however it is seeded.
	std::vector<int> sorted = first;
	std::sort(sorted.begin(), sorted.end());
	EXPECT_EQ(sorted, original);
}

TEST(SeedTest, ShuffleLeavesShortSequencesAlone) {
	std::vector<int> empty;
	seed::shuffle(empty, 1, "x");
	EXPECT_TRUE(empty.empty());

	std::vector<int> single = {7};
	seed::shuffle(single, 1, "x");
	EXPECT_EQ(single, std::vector<int>({7}));
}
