#include "iota/core/update_checker.hpp"

#include "iota/core/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace iota::core {

namespace {

std::string_view trimAsciiWhitespace(std::string_view sv) noexcept {
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
    sv.remove_suffix(1);
  }
  return sv;
}

bool parseNonNegInt(std::string_view sv, int& outVal) noexcept {
  if (sv.empty()) {
    return false;
  }
  for (char c : sv) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  int v = 0;
  const auto res = std::from_chars(sv.data(), sv.data() + sv.size(), v);
  if (res.ec != std::errc{} || res.ptr != sv.data() + sv.size()) {
    return false;
  }
  outVal = v;
  return true;
}

bool isAllDigits(std::string_view sv) noexcept {
  if (sv.empty()) {
    return false;
  }
  for (char c : sv) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

std::vector<std::string_view> splitDots(std::string_view sv) {
  std::vector<std::string_view> parts;
  size_t pos = 0;
  while (pos <= sv.size()) {
    const size_t dot = sv.find('.', pos);
    if (dot == std::string_view::npos) {
      parts.push_back(sv.substr(pos));
      break;
    }
    parts.push_back(sv.substr(pos, dot - pos));
    pos = dot + 1;
  }
  return parts;
}

int compareNumericStrings(std::string_view a, std::string_view b) noexcept {
  auto stripLeadingZeros = [](std::string_view s) {
    while (s.size() > 1 && s.front() == '0') {
      s.remove_prefix(1);
    }
    return s;
  };
  const std::string_view na = stripLeadingZeros(a);
  const std::string_view nb = stripLeadingZeros(b);
  if (na.size() != nb.size()) {
    return (na.size() < nb.size()) ? -1 : 1;
  }
  if (na == nb) {
    return 0;
  }
  return (na < nb) ? -1 : 1;
}

bool endsWithIgnoreCase(std::string_view str, std::string_view suffix) noexcept {
  if (str.size() < suffix.size()) {
    return false;
  }
  const std::string_view tail = str.substr(str.size() - suffix.size());
  for (size_t i = 0; i < suffix.size(); ++i) {
    const unsigned char ca = static_cast<unsigned char>(tail[i]);
    const unsigned char cb = static_cast<unsigned char>(suffix[i]);
    if (std::tolower(ca) != std::tolower(cb)) {
      return false;
    }
  }
  return true;
}

bool containsIgnoreCase(std::string_view haystack, std::string_view needle) noexcept {
  if (needle.empty() || needle.size() > haystack.size()) {
    return false;
  }
  const size_t limit = haystack.size() - needle.size();
  for (size_t i = 0; i <= limit; ++i) {
    bool match = true;
    for (size_t j = 0; j < needle.size(); ++j) {
      if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
          std::tolower(static_cast<unsigned char>(needle[j]))) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

bool isMcpAssetName(std::string_view name) noexcept {
  return containsIgnoreCase(name, "-mcp") ||
         containsIgnoreCase(name, "_mcp") ||
         containsIgnoreCase(name, "mcp-setup") ||
         containsIgnoreCase(name, "iota_mcp");
}

#ifdef _WIN32

std::wstring utf8ToWideString(std::string_view utf8) {
  if (utf8.empty()) {
    return {};
  }
  const int needed = MultiByteToWideChar(
      CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) {
    return {};
  }
  std::wstring wide(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(
      CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
  return wide;
}

struct WinHttpHandle {
  HINTERNET h = nullptr;
  WinHttpHandle() = default;
  explicit WinHttpHandle(HINTERNET handle) noexcept : h(handle) {}
  ~WinHttpHandle() {
    if (h != nullptr) {
      WinHttpCloseHandle(h);
    }
  }
  WinHttpHandle(const WinHttpHandle&) = delete;
  WinHttpHandle& operator=(const WinHttpHandle&) = delete;
  explicit operator bool() const noexcept { return h != nullptr; }
};

struct HttpFetchResult {
  bool networkOk = false;
  DWORD statusCode = 0;
  std::string body;
  std::string errorMessage;
};

HttpFetchResult fetchLatestReleaseJsonWinHttp(const std::string& owner,
                                              const std::string& repo,
                                              const std::string& currentVersion,
                                              const std::atomic<bool>& cancelled) {
  HttpFetchResult res;
  const std::wstring userAgent = L"IotaStudio/" + utf8ToWideString(currentVersion.empty() ? "1.0.0" : currentVersion);
  WinHttpHandle session(WinHttpOpen(
      userAgent.c_str(),
      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS,
      0));
  if (!session) {
    res.errorMessage = "Failed to initialize Windows HTTP session.";
    return res;
  }

  WinHttpSetTimeouts(session.h, 5000, 5000, 5000, 8000);

  if (cancelled.load(std::memory_order_acquire)) {
    res.errorMessage = "Cancelled";
    return res;
  }

  WinHttpHandle connect(WinHttpConnect(session.h, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
  if (!connect) {
    res.errorMessage = "Unable to reach api.github.com.";
    return res;
  }

  const std::string pathUtf8 = "/repos/" + owner + "/" + repo + "/releases/latest";
  const std::wstring pathWide = utf8ToWideString(pathUtf8);
  WinHttpHandle request(WinHttpOpenRequest(
      connect.h,
      L"GET",
      pathWide.c_str(),
      nullptr,
      WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES,
      WINHTTP_FLAG_SECURE));
  if (!request) {
    res.errorMessage = "Failed to open GitHub HTTPS request.";
    return res;
  }

  const wchar_t* headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
  if (!WinHttpSendRequest(request.h, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
    res.errorMessage = "Could not contact GitHub Releases (offline or connection blocked).";
    return res;
  }

  if (cancelled.load(std::memory_order_acquire)) {
    res.errorMessage = "Cancelled";
    return res;
  }

  if (!WinHttpReceiveResponse(request.h, nullptr)) {
    res.errorMessage = "Timed out or failed reading response from GitHub Releases.";
    return res;
  }

  DWORD statusCode = 0;
  DWORD statusSize = sizeof(statusCode);
  if (!WinHttpQueryHeaders(
          request.h,
          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX,
          &statusCode,
          &statusSize,
          WINHTTP_NO_HEADER_INDEX)) {
    res.errorMessage = "Missing HTTP status code from GitHub Releases.";
    return res;
  }

  res.networkOk = true;
  res.statusCode = statusCode;

  constexpr size_t kMaxResponseBytes = 1024u * 1024u;
  std::string body;
  for (;;) {
    if (cancelled.load(std::memory_order_acquire)) {
      res.errorMessage = "Cancelled";
      return res;
    }
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(request.h, &avail)) {
      res.errorMessage = "Failed while reading update payload.";
      res.networkOk = false;
      return res;
    }
    if (avail == 0) {
      break;
    }
    const DWORD toRead = std::min<DWORD>(avail, 16384);
    if (body.size() + static_cast<size_t>(toRead) > kMaxResponseBytes) {
      res.errorMessage = "GitHub release response exceeded size limit.";
      res.networkOk = false;
      return res;
    }
    const size_t oldSize = body.size();
    body.resize(oldSize + static_cast<size_t>(toRead));
    DWORD bytesRead = 0;
    if (!WinHttpReadData(request.h, body.data() + oldSize, toRead, &bytesRead)) {
      res.errorMessage = "Interrupted while reading update payload.";
      res.networkOk = false;
      return res;
    }
    body.resize(oldSize + static_cast<size_t>(bytesRead));
  }

  res.body = std::move(body);
  return res;
}

#endif  // _WIN32

}  // namespace

std::optional<SemVer> parseSemVer(std::string_view text) {
  std::string_view sv = trimAsciiWhitespace(text);
  if (!sv.empty() && (sv.front() == 'v' || sv.front() == 'V')) {
    sv.remove_prefix(1);
  }
  if (sv.empty()) {
    return std::nullopt;
  }

  // Strip optional build metadata (+...)
  if (const size_t plusPos = sv.find('+'); plusPos != std::string_view::npos) {
    sv = sv.substr(0, plusPos);
  }

  // Split optional prerelease (-...)
  std::string_view pre;
  if (const size_t dashPos = sv.find('-'); dashPos != std::string_view::npos) {
    pre = sv.substr(dashPos + 1);
    sv = sv.substr(0, dashPos);
    if (pre.empty()) {
      return std::nullopt;
    }
  }

  const auto parts = splitDots(sv);
  if (parts.size() < 2 || parts.size() > 3) {
    return std::nullopt;
  }

  SemVer out;
  if (!parseNonNegInt(parts[0], out.major) || !parseNonNegInt(parts[1], out.minor)) {
    return std::nullopt;
  }
  if (parts.size() == 3 && !parseNonNegInt(parts[2], out.patch)) {
    return std::nullopt;
  }
  if (!pre.empty()) {
    const auto preParts = splitDots(pre);
    for (std::string_view ident : preParts) {
      if (ident.empty()) {
        return std::nullopt;
      }
      for (char c : ident) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') {
          return std::nullopt;
        }
      }
    }
    out.prerelease = std::string(pre);
  }
  return out;
}

int compareSemVer(const SemVer& a, const SemVer& b) {
  if (a.major != b.major) return (a.major < b.major) ? -1 : 1;
  if (a.minor != b.minor) return (a.minor < b.minor) ? -1 : 1;
  if (a.patch != b.patch) return (a.patch < b.patch) ? -1 : 1;

  if (a.prerelease.empty() && b.prerelease.empty()) return 0;
  if (a.prerelease.empty()) return 1;   // Stable > prerelease
  if (b.prerelease.empty()) return -1;  // Prerelease < stable

  const auto aIdents = splitDots(a.prerelease);
  const auto bIdents = splitDots(b.prerelease);
  const size_t minCount = std::min(aIdents.size(), bIdents.size());
  for (size_t i = 0; i < minCount; ++i) {
    const std::string_view ia = aIdents[i];
    const std::string_view ib = bIdents[i];
    const bool numA = isAllDigits(ia);
    const bool numB = isAllDigits(ib);
    if (numA && numB) {
      const int cmp = compareNumericStrings(ia, ib);
      if (cmp != 0) return cmp;
    } else if (numA != numB) {
      return numA ? -1 : 1;  // Numeric identifiers have lower precedence than non-numeric
    } else if (ia != ib) {
      return (ia < ib) ? -1 : 1;
    }
  }
  if (aIdents.size() != bIdents.size()) {
    return (aIdents.size() < bIdents.size()) ? -1 : 1;
  }
  return 0;
}

bool isNewerVersion(std::string_view current, std::string_view candidate) {
  const auto curVer = parseSemVer(current);
  const auto candVer = parseSemVer(candidate);
  if (!curVer || !candVer) {
    return false;
  }
  return compareSemVer(*candVer, *curVer) > 0;
}

bool isSafeHttpsUrl(std::string_view url) {
  constexpr std::string_view kPrefix = "https://";
  if (url.size() <= kPrefix.size() || url.substr(0, kPrefix.size()) != kPrefix) {
    return false;
  }
  for (unsigned char c : url) {
    if (c <= 0x20 || c >= 0x7F || c == '<' || c == '>' || c == '"' || c == '\'' || c == '\\' || c == '`') {
      return false;
    }
  }
  return true;
}

std::optional<ReleaseInfo> parseGitHubReleaseJson(std::string_view jsonBody,
                                                  std::string_view currentVersion,
                                                  std::string_view fallbackReleasesUrl,
                                                  std::string* outError,
                                                  bool preferMcpAsset) {
  const nlohmann::json doc = nlohmann::json::parse(jsonBody, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) {
    if (outError) {
      *outError = "Invalid JSON response from GitHub Releases API.";
    }
    return std::nullopt;
  }

  auto readString = [&](const char* key) -> std::string {
    const auto it = doc.find(key);
    if (it != doc.end() && it->is_string()) {
      return sanitizeUtf8(it->get_ref<const std::string&>());
    }
    return {};
  };

  ReleaseInfo info;
  info.tagName = readString("tag_name");
  if (info.tagName.empty()) {
    if (outError) {
      *outError = "GitHub release JSON did not include a tag_name.";
    }
    return std::nullopt;
  }

  if (const auto parsed = parseSemVer(info.tagName)) {
    info.version = std::to_string(parsed->major) + "." +
                   std::to_string(parsed->minor) + "." +
                   std::to_string(parsed->patch);
    if (!parsed->prerelease.empty()) {
      info.version += "-" + parsed->prerelease;
    }
  } else {
    info.version = info.tagName;
  }

  info.title = readString("name");
  if (info.title.empty()) {
    info.title = info.tagName;
  }

  const std::string rawHtmlUrl = readString("html_url");
  info.htmlUrl = isSafeHttpsUrl(rawHtmlUrl)
                     ? rawHtmlUrl
                     : std::string(fallbackReleasesUrl);

  info.publishedAt = readString("published_at");
  info.releaseNotes = readString("body");

  if (const auto it = doc.find("prerelease"); it != doc.end() && it->is_boolean()) {
    info.isPrerelease = it->get<bool>();
  }
  if (const auto it = doc.find("draft"); it != doc.end() && it->is_boolean()) {
    info.isDraft = it->get<bool>();
  }

  // Pick preferred Windows release binary asset (.exe > .zip > first valid HTTPS asset),
  // prioritizing the running edition (MCP vs Standard) while falling back cleanly.
  int bestScore = -1;
  if (const auto assetsIt = doc.find("assets"); assetsIt != doc.end() && assetsIt->is_array()) {
    for (const auto& asset : *assetsIt) {
      if (!asset.is_object()) {
        continue;
      }
      const auto urlIt = asset.find("browser_download_url");
      if (urlIt == asset.end() || !urlIt->is_string()) {
        continue;
      }
      const std::string url = sanitizeUtf8(urlIt->get_ref<const std::string&>());
      if (!isSafeHttpsUrl(url)) {
        continue;
      }
      std::string name;
      if (const auto nameIt = asset.find("name"); nameIt != asset.end() && nameIt->is_string()) {
        name = sanitizeUtf8(nameIt->get_ref<const std::string&>());
      }
      const bool editionMatch = (isMcpAssetName(name) == preferMcpAsset);
      int score = 1;
      if (endsWithIgnoreCase(name, ".exe")) {
        score = editionMatch ? 5 : 4;
      } else if (endsWithIgnoreCase(name, ".zip")) {
        score = editionMatch ? 3 : 2;
      }
      if (score > bestScore) {
        bestScore = score;
        info.assetName = name;
        info.assetDownloadUrl = url;
        if (const auto szIt = asset.find("size"); szIt != asset.end() && szIt->is_number_unsigned()) {
          info.assetSizeBytes = szIt->get<uint64_t>();
        } else {
          info.assetSizeBytes = 0;
        }
      }
    }
  }

  const std::string_view candVer = info.version.empty() ? std::string_view(info.tagName)
                                                      : std::string_view(info.version);
  info.updateAvailable = !info.isDraft && !info.isPrerelease && isNewerVersion(currentVersion, candVer);
  return info;
}

struct UpdateChecker::SharedState {
  mutable std::mutex dataMutex;
  UpdateCheckSnapshot snapshot;
  std::atomic<bool> inFlight{false};
  std::atomic<bool> cancelled{false};
};

UpdateChecker::~UpdateChecker() {
  cancel();
}

void UpdateChecker::cancel() {
  std::shared_ptr<SharedState> current;
  {
    std::lock_guard<std::mutex> lock(statePtrMutex_);
    current = state_;
  }
  if (current) {
    current->cancelled.store(true, std::memory_order_release);
  }
}

UpdateCheckSnapshot UpdateChecker::snapshot() const {
  std::shared_ptr<SharedState> current;
  {
    std::lock_guard<std::mutex> lock(statePtrMutex_);
    current = state_;
  }
  if (!current) {
    return UpdateCheckSnapshot{};
  }
  std::lock_guard<std::mutex> dataLock(current->dataMutex);
  return current->snapshot;
}

void UpdateChecker::startCheckAsync(std::string currentVersion,
                                    std::string owner,
                                    std::string repo,
                                    bool isManual,
                                    bool preferMcpAsset) {
  std::shared_ptr<SharedState> nextState;
  {
    std::lock_guard<std::mutex> lock(statePtrMutex_);
    if (state_ && state_->inFlight.load(std::memory_order_acquire) &&
        !state_->cancelled.load(std::memory_order_acquire)) {
      if (isManual) {
        std::lock_guard<std::mutex> dataLock(state_->dataMutex);
        state_->snapshot.isManualCheck = true;
      }
      return;
    }

    nextState = std::make_shared<SharedState>();
    nextState->inFlight.store(true, std::memory_order_release);
    nextState->snapshot.status = UpdateCheckStatus::Checking;
    nextState->snapshot.currentVersion = currentVersion;
    nextState->snapshot.isManualCheck = isManual;
    nextState->snapshot.errorMessage.clear();
    nextState->snapshot.latestRelease = ReleaseInfo{};
    state_ = nextState;
  }

  std::thread([nextState,
               curVer = std::move(currentVersion),
               repoOwner = std::move(owner),
               repoName = std::move(repo),
               preferMcpAsset]() {
    auto finishError = [&](std::string msg) {
      if (!nextState->cancelled.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> dataLock(nextState->dataMutex);
        nextState->snapshot.status = UpdateCheckStatus::Error;
        nextState->snapshot.errorMessage = std::move(msg);
      }
      nextState->inFlight.store(false, std::memory_order_release);
    };

#ifdef _WIN32
    const HttpFetchResult fetch = fetchLatestReleaseJsonWinHttp(
        repoOwner, repoName, curVer, nextState->cancelled);
    if (nextState->cancelled.load(std::memory_order_acquire)) {
      nextState->inFlight.store(false, std::memory_order_release);
      return;
    }
    if (!fetch.networkOk) {
      finishError(fetch.errorMessage.empty() ? "Update check failed." : fetch.errorMessage);
      return;
    }
    if (fetch.statusCode == 404) {
      finishError("No published releases found on GitHub.");
      return;
    }
    if (fetch.statusCode == 403 || fetch.statusCode == 429) {
      finishError("GitHub API rate limit reached. Try again later.");
      return;
    }
    if (fetch.statusCode != 200) {
      finishError("GitHub API returned HTTP " + std::to_string(fetch.statusCode) + ".");
      return;
    }

    const std::string fallbackUrl = "https://github.com/" + repoOwner + "/" + repoName + "/releases";
    std::string parseErr;
    const auto parsed = parseGitHubReleaseJson(fetch.body, curVer, fallbackUrl, &parseErr, preferMcpAsset);
    if (!parsed) {
      finishError(parseErr.empty() ? "Failed to parse GitHub release metadata." : parseErr);
      return;
    }

    if (!nextState->cancelled.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> dataLock(nextState->dataMutex);
      nextState->snapshot.latestRelease = *parsed;
      nextState->snapshot.status = parsed->updateAvailable
                                       ? UpdateCheckStatus::UpdateAvailable
                                       : UpdateCheckStatus::UpToDate;
      nextState->snapshot.errorMessage.clear();
    }
    nextState->inFlight.store(false, std::memory_order_release);
#else
    (void)repoOwner;
    (void)repoName;
    (void)curVer;
    finishError("Automatic update checks are only supported on Windows.");
#endif
  }).detach();
}

}  // namespace iota::core
