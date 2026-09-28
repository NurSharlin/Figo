#ifndef SEARCH_FILTER_H
#define SEARCH_FILTER_H

#include <stdint.h>
#include <string.h>

// Which rows of a provider's search response are worth showing, and what kind
// of screen each one becomes.
//
// Dependency-free ON PURPOSE - <stdint.h> and <string.h>, nothing else - so it
// can be compiled and unit-tested on a host with a plain g++. These rules fail
// silently on the device (a junk row appears, and nothing says why), which is
// exactly the shape of bug worth pinning down off-device. Don't add includes.
// Same contract as SleepWindow.h.

// ScreenConfig::symbol is char[16] (SharedState.h), so 15 usable characters.
// Yahoo really does return 19-character option symbols for an ordinary query.
#define SEARCH_SYMBOL_MAX 15

// CoinGecko's /search is ordered by market cap and has a very long tail: a
// query for "apple" returns six tokenized Apple wrappers, and "oil" returns a
// coin called Skibidi Toilet. Rank is the only cheap signal that separates a
// real coin from that, and the top 500 covers everything a desk display wants.
#define SEARCH_CG_RANK_MAX 500

// Deliberately NOT ScreenType: that lives in SharedState.h, which pulls in
// Arduino and FreeRTOS. DataFetcher maps this to a ScreenType at the boundary.
enum SearchSource : uint8_t {
  SEARCH_SOURCE_STOCK = 0,
  SEARCH_SOURCE_COMMODITY = 1,
  SEARCH_SOURCE_CRYPTO = 2,
};

// '&' and '/' are rejected rather than escaped: urlEncodeSymbol() in
// DataFetcher.cpp escapes only '^', '=' and space, so either character would
// break the URL the screen is fetched with - long after this list was shown.
inline bool searchSymbolAcceptable(const char *symbol) {
  if (symbol == nullptr || symbol[0] == '\0') return false;
  size_t n = strlen(symbol);
  if (n > SEARCH_SYMBOL_MAX) return false;
  for (size_t i = 0; i < n; i++) {
    if (symbol[i] == '&' || symbol[i] == '/') return false;
  }
  return true;
}

inline bool searchQuoteTypeAcceptable(const char *quoteType) {
  if (quoteType == nullptr) return false;
  // Options expire, carry a strike in the symbol, and are never what someone
  // typing a company name meant.
  return strcmp(quoteType, "OPTION") != 0;
}

// Yahoo prices everything it returns, futures included, so this only decides
// which label the screen wears - Stock or Commodity. A CRYPTOCURRENCY row from
// Yahoo (BTC-USD) is a YAHOO instrument and must map to STOCK, not CRYPTO:
// getting this wrong would send a Yahoo symbol to CoinGecko's simple/price.
inline SearchSource searchSourceForQuoteType(const char *quoteType) {
  if (quoteType != nullptr && strcmp(quoteType, "FUTURE") == 0) {
    return SEARCH_SOURCE_COMMODITY;
  }
  return SEARCH_SOURCE_STOCK;
}

// CoinGecko sends null for an unranked coin, which arrives here as 0.
inline bool searchCoinRankAcceptable(int32_t rank) {
  return rank > 0 && rank <= SEARCH_CG_RANK_MAX;
}

#endif  // SEARCH_FILTER_H
