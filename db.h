#include <stdint.h>
#include <math.h>

#include <set>
#include <map>
#include <vector>
#include <deque>

#include "netbase.h"
#include "protocol.h"
#include "util.h"

#define MIN_RETRY 1000

// REQUIRE Protocol Version
#define REQUIRE_VERSION 80012
/*   	"version": 150000,		// Bitcoin
  	"protocolversion": 70015,
  	"walletversion": 60000,

    "version" : 90500,			// Kalkulus
    "protocolversion" : 70002,
    "walletversion" : 60000,

    "version" : 90700,                  // Kalkulus - Fork #1
    "protocolversion" : 70002,
    "walletversion" : 60000,

    "version" : 90803,			// Kalkulus - Fork #2
    "protocolversion" : 70004,
    "walletversion" : 60000,

	version:
		The version number of this kalkulus-qt or kalkulusd program itself. Both of are equivalent. -qt is simply the graphical user interface version

	protocolversion:
		The version of the kalkulus network protocol supported by this client.

	walletversion:
		The version of the wallet.dat file. Wallet.dat contains kalkulus addresses and public & private key pairs for these addresses. There is additional data on the wallet. Care must be taken to not restore from an old wallet backup. New addresses generated in the wallet since the old backup was made will not exist in the old backup! ( Source: https://en.bitcoin.it/wiki/Wallet )

*/

// Bitcoin: If testnet require 10,000 blocks : 11381 on testnet4 as of Nov26'17, otherwise 360,000

// Kalkulus: If testnet, require 0 blocks  otherwise 465,639 (as of 1530104867: Wed Jun 27 13:07:47 UTC 2018)
// Kalkulus block 465639 hash: 3a7faa44a2898f3d9be0de967904640e23026fd9fa0cee4ae89c20c7030bfdd5
static inline int GetRequireHeight(const bool testnet = fTestNet)
{
    return testnet ? 0 : 250000;
}

std::string static inline ToString(const CService &ip) {
  std::string str = ip.ToString();
  while (str.size() < 22) str += ' ';
  return str;
}

class CAddrStat {
private:
  float weight;
  float count;
  float reliability;
public:
  CAddrStat() : weight(0), count(0), reliability(0) {}

  void Update(bool good, int64 age, double tau) {
    double f =  exp(-age/tau);
    reliability = reliability * f + (good ? (1.0-f) : 0);
    count = count * f + 1;
    weight = weight * f + (1.0-f);
  }

  IMPLEMENT_SERIALIZE (
    READWRITE(weight);
    READWRITE(count);
    READWRITE(reliability);
  )

  friend class CAddrInfo;
};

class CAddrReport {
public:
  CService ip;
  int clientVersion;
  int blocks;
  double uptime[5];
  std::string clientSubVersion;
  int64_t lastSuccess;
  bool fGood;
  uint64_t services;
};


class CAddrInfo {
private:
  CService ip;
  uint64_t services;
  int64 lastTry;
  int64 ourLastTry;
  int64 ourLastSuccess;
  int64 ignoreTill;
  CAddrStat stat2H;
  CAddrStat stat8H;
  CAddrStat stat1D;
  CAddrStat stat1W;
  CAddrStat stat1M;
  int clientVersion;
  int blocks;
  int total;
  int success;
  std::string clientSubVersion;
public:
  CAddrInfo() : services(0), lastTry(0), ourLastTry(0), ourLastSuccess(0), ignoreTill(0), clientVersion(0), blocks(0), total(0), success(0) {}

  CAddrReport GetReport() const {
    CAddrReport ret;
    ret.ip = ip;
    ret.clientVersion = clientVersion;
    ret.clientSubVersion = clientSubVersion;
    ret.blocks = blocks;
    ret.uptime[0] = stat2H.reliability;
    ret.uptime[1] = stat8H.reliability;
    ret.uptime[2] = stat1D.reliability;
    ret.uptime[3] = stat1W.reliability;
    ret.uptime[4] = stat1M.reliability;
    ret.lastSuccess = ourLastSuccess;
    ret.fGood = IsGood();
    ret.services = services;
    return ret;
  }

  // Node Discriminator Function
  bool IsGood() const {
    if (ip.GetPort() != GetDefaultPort()) return false;
    if (!(services & NODE_NETWORK)) return false;
    if (!ip.IsRoutable()) return false;
    if (clientVersion && clientVersion < REQUIRE_VERSION) return false;
    if (blocks && blocks < GetRequireHeight()) return false;

    /*
	https://stackoverflow.com/questions/2340281/check-if-a-string-contains-a-string-in-c
	if (s1.find(s2) != std::string::npos) {
	    std::cout << "found!" << '\n';
	}
	Note: "found!" will be printed if s2 is a substring of s1, both s1 and s2 are of type std::string.
    */
    // Hack: only return 0.9.8.3 nodes; no 0.9.7.x, nor 0.9.5 nor 0.9.4 nor 0.9.2
    if (clientSubVersion.find(7) != std::string::npos ) return false;
    if (clientSubVersion.find(5) != std::string::npos ) {
		// Audit Trail of decisions, logging facility ....
	        // printf("Rejecting old subversion: 9.5.\n");
		return false;
	};
    if (clientSubVersion.find(4) != std::string::npos ) return false;
    if (clientSubVersion.find(2) != std::string::npos ) return false;

    if (total <= 3 && success * 2 >= total) return true;

    if (stat2H.reliability > 0.85 && stat2H.count > 2) return true;
    if (stat8H.reliability > 0.70 && stat8H.count > 4) return true;
    if (stat1D.reliability > 0.55 && stat1D.count > 8) return true;
    if (stat1W.reliability > 0.45 && stat1W.count > 16) return true;
    if (stat1M.reliability > 0.35 && stat1M.count > 32) return true;

    return false;
  }

  int GetBanTime() const {
    if (IsGood()) return 0;
    //  Kalkulus clientVersion ("Version") 90803  (previous cutoff: 90700 )
    //    if (clientVersion && clientVersion < 31900) { return 604800; }   // Bitcoin
    // 1 week = 604800 seconds
    if (clientVersion && clientVersion < 90803) { return 604800; }
    if (stat1M.reliability - stat1M.weight + 1.0 < 0.15 && stat1M.count > 32) { return 30*86400; }
    if (stat1W.reliability - stat1W.weight + 1.0 < 0.10 && stat1W.count > 16) { return 7*86400; }
    if (stat1D.reliability - stat1D.weight + 1.0 < 0.05 && stat1D.count > 8) { return 1*86400; }
    return 0;
  }
  int GetIgnoreTime() const {
    if (IsGood()) return 0;
    if (stat1M.reliability - stat1M.weight + 1.0 < 0.20 && stat1M.count > 2) { return 10*86400; }
    if (stat1W.reliability - stat1W.weight + 1.0 < 0.16 && stat1W.count > 2)  { return 3*86400; }
    if (stat1D.reliability - stat1D.weight + 1.0 < 0.12 && stat1D.count > 2)  { return 8*3600; }
    if (stat8H.reliability - stat8H.weight + 1.0 < 0.08 && stat8H.count > 2)  { return 2*3600; }
    return 0;
  }

  void Update(bool good);

  friend class CAddrDb;

  IMPLEMENT_SERIALIZE (
    unsigned char version = 4;
    READWRITE(version);
    READWRITE(ip);
    READWRITE(services);
    READWRITE(lastTry);
    unsigned char tried = ourLastTry != 0;
    READWRITE(tried);
    if (tried) {
      READWRITE(ourLastTry);
      READWRITE(ignoreTill);
      READWRITE(stat2H);
      READWRITE(stat8H);
      READWRITE(stat1D);
      READWRITE(stat1W);
      if (version >= 1)
          READWRITE(stat1M);
      else
          if (!fWrite)
              *((CAddrStat*)(&stat1M)) = stat1W;
      READWRITE(total);
      READWRITE(success);
      READWRITE(clientVersion);
      if (version >= 2)
          READWRITE(clientSubVersion);
      if (version >= 3)
          READWRITE(blocks);
      if (version >= 4)
          READWRITE(ourLastSuccess);
    }
  )
};

class CAddrDbStats {
public:
  int nBanned;
  int nAvail;
  int nTracked;
  int nNew;
  int nGood;
  int nAge;
};

struct CServiceResult {
    CService service;
    bool fGood;
    int nBanTime;
    int nHeight;
    int nClientV;
    std::string strClientV;
    int64 ourLastSuccess;
};

//             seen nodes
//            /          \
// (a) banned nodes       available nodes--------------
//                       /       |                     \
//               tracked nodes   (b) unknown nodes   (e) active nodes
//              /           \
//     (d) good nodes   (c) non-good nodes

class CAddrDb {
private:
  mutable CCriticalSection cs;
  int nId; // number of address id's
  std::map<int, CAddrInfo> idToInfo; // map address id to address info (b,c,d,e)
  std::map<CService, int> ipToId; // map ip to id (b,c,d,e)
  std::deque<int> ourId; // sequence of tried nodes, in order we have tried connecting to them (c,d)
  std::set<int> unkId; // set of nodes not yet tried (b)
  std::set<int> goodId; // set of good nodes  (d, good e)
  int nDirty;

protected:
  // internal routines that assume proper locks are acquired
  void Add_(const CAddress &addr, bool force);   // add an address
  bool Get_(CServiceResult &ip, int& wait);      // get an IP to test (must call Good_, Bad_, or Skipped_ on result afterwards)
  bool GetMany_(std::vector<CServiceResult> &ips, int max, int& wait);
  void Good_(const CService &ip, int clientV, std::string clientSV, int blocks); // mark an IP as good (must have been returned by Get_)
  void Bad_(const CService &ip, int ban);  // mark an IP as bad (and optionally ban it) (must have been returned by Get_)
  void Skipped_(const CService &ip);       // mark an IP as skipped (must have been returned by Get_)
  int Lookup_(const CService &ip);         // look up id of an IP
  void GetIPs_(std::set<CNetAddr>& ips, uint64_t requestedFlags, int max, const bool *nets); // get a random set of IPs (shared lock only)

public:
  std::map<CService, time_t> banned; // nodes that are banned, with their unban time (a)

  void GetStats(CAddrDbStats &stats) {
    SHARED_CRITICAL_BLOCK(cs) {
      stats.nBanned = banned.size();
      stats.nAvail = idToInfo.size();
      stats.nTracked = ourId.size();
      stats.nGood = goodId.size();
      stats.nNew = unkId.size();
      stats.nAge = time(NULL) - idToInfo[ourId[0]].ourLastTry;
    }
  }

  void ResetIgnores() {
      for (std::map<int, CAddrInfo>::iterator it = idToInfo.begin(); it != idToInfo.end(); it++) {
           (*it).second.ignoreTill = 0;
      }
  }

  std::vector<CAddrReport> GetAll() {
    std::vector<CAddrReport> ret;
    SHARED_CRITICAL_BLOCK(cs) {
      for (std::deque<int>::const_iterator it = ourId.begin(); it != ourId.end(); it++) {
        const CAddrInfo &info = idToInfo[*it];
        if (info.success > 0) {
          ret.push_back(info.GetReport());
        }
      }
    }
    return ret;
  }

  // serialization code
  // format:
  //   nVersion (0 for now)
  //   n (number of ips in (b,c,d))
  //   CAddrInfo[n]
  //   banned
  // acquires a shared lock (this does not suffice for read mode, but we assume that only happens at startup, single-threaded)
  // this way, dumping does not interfere with GetIPs_, which is called from the DNS thread
  IMPLEMENT_SERIALIZE (({
    int nVersion = 0;
    READWRITE(nVersion);
    SHARED_CRITICAL_BLOCK(cs) {
      if (fWrite) {
        CAddrDb *db = const_cast<CAddrDb*>(this);
        int n = ourId.size() + unkId.size();
        READWRITE(n);
        for (std::deque<int>::const_iterator it = ourId.begin(); it != ourId.end(); it++) {
          std::map<int, CAddrInfo>::iterator ci = db->idToInfo.find(*it);
          READWRITE((*ci).second);
        }
        for (std::set<int>::const_iterator it = unkId.begin(); it != unkId.end(); it++) {
          std::map<int, CAddrInfo>::iterator ci = db->idToInfo.find(*it);
          READWRITE((*ci).second);
        }
      } else {
        CAddrDb *db = const_cast<CAddrDb*>(this);
        db->nId = 0;
        int n;
        READWRITE(n);
        for (int i=0; i<n; i++) {
          CAddrInfo info;
          READWRITE(info);
          if (!info.GetBanTime()) {
            int id = db->nId++;
            db->idToInfo[id] = info;
            db->ipToId[info.ip] = id;
            if (info.ourLastTry) {
              db->ourId.push_back(id);
              if (info.IsGood()) db->goodId.insert(id);
            } else {
              db->unkId.insert(id);
            }
          }
        }
        db->nDirty++;
      }
      READWRITE(banned);
    }
  });)

  void Add(const CAddress &addr, bool fForce = false) {
    CRITICAL_BLOCK(cs)
      Add_(addr, fForce);
  }
  void Add(const std::vector<CAddress> &vAddr, bool fForce = false) {
    CRITICAL_BLOCK(cs)
      for (int i=0; i<vAddr.size(); i++)
        Add_(vAddr[i], fForce);
  }
  void Good(const CService &addr, int clientVersion, std::string clientSubVersion, int blocks) {
    CRITICAL_BLOCK(cs)
      Good_(addr, clientVersion, clientSubVersion, blocks);
  }
  void Skipped(const CService &addr) {
    CRITICAL_BLOCK(cs)
      Skipped_(addr);
  }
  void Bad(const CService &addr, int ban = 0) {
    CRITICAL_BLOCK(cs)
      Bad_(addr, ban);
  }
  bool Get(CServiceResult &ip, int& wait) {
    CRITICAL_BLOCK(cs)
      return Get_(ip, wait);
  }
  void GetMany(std::vector<CServiceResult> &ips, int max, int& wait) {
    CRITICAL_BLOCK(cs) {
      while (max > 0) {
          CServiceResult ip = {};
          if (!Get_(ip, wait))
              return;
          ips.push_back(ip);
          max--;
      }
    }
  }
  void ResultMany(const std::vector<CServiceResult> &ips) {
    CRITICAL_BLOCK(cs) {
      for (int i=0; i<ips.size(); i++) {
        if (ips[i].fGood) {
          Good_(ips[i].service, ips[i].nClientV, ips[i].strClientV, ips[i].nHeight);
        } else {
          Bad_(ips[i].service, ips[i].nBanTime);
        }
      }
    }
  }
  void GetIPs(std::set<CNetAddr>& ips, uint64_t requestedFlags, int max, const bool *nets) {
    SHARED_CRITICAL_BLOCK(cs)
      GetIPs_(ips, requestedFlags, max, nets);
  }
};
