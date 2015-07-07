// Copyright (c) 2013, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.
#ifndef KUDU_TPCH_RPC_LINE_ITEM_DAO_H
#define KUDU_TPCH_RPC_LINE_ITEM_DAO_H

#include <boost/function.hpp>
#include <set>
#include <string>
#include <tr1/memory>
#include <utility>
#include <vector>

#include "kudu/benchmarks/tpch/tpch-schemas.h"
#include "kudu/client/client.h"
#include "kudu/client/row_result.h"
#include "kudu/util/locks.h"
#include "kudu/util/monotime.h"
#include "kudu/util/semaphore.h"

namespace kudu {

class RpcLineItemDAO {
 public:
  class Scanner;

  RpcLineItemDAO(const std::string& master_address,
                 const std::string& table_name,
                 int batch_size,
                 int mstimeout = 5000,
                 const std::vector<std::string>& tablet_splits = std::vector<std::string>());
  ~RpcLineItemDAO();
  void WriteLine(boost::function<void(KuduPartialRow*)> f);
  void MutateLine(boost::function<void(KuduPartialRow*)> f);
  void Init();
  void FinishWriting();

  // Deletes previous scanner if one is open. 'query_schema' is copied internally and can safely
  // be discarded after this call.
  void OpenScanner(const client::KuduSchema& query_schema,
                   gscoped_ptr<Scanner>* scanner);
  // Calls OpenScanner with the tpch1 query parameters.
  void OpenTpch1Scanner(gscoped_ptr<Scanner>* scanner);

  // Opens a scanner with the TPCH Q1 projection and filter, plus range filter to only
  // select rows in the given order key range.
  void OpenTpch1ScannerForOrderKeyRange(int64_t min_orderkey, int64_t max_orderkey,
                                        gscoped_ptr<Scanner>* scanner);
  bool IsTableEmpty();

  class Scanner {
   public:
    ~Scanner() {}

    // Return true if there are more rows left in the scanner.
    bool HasMore();

    // Return the next batch of rows into '*rows'. Any existing data is cleared.
    void GetNext(std::vector<client::KuduRowResult> *rows);

   private:
    friend class RpcLineItemDAO;
    Scanner() {}

    // Keeps a copy of the KuduSchema provided by OpenScanner() to ensure the schema's
    // liveness while scanning.
    gscoped_ptr<client::KuduSchema> projection_;
    gscoped_ptr<client::KuduScanner> scanner_;
  };

 private:
  static const Slice kScanUpperBound;

  void FlushIfBufferFull();
  void OpenScanner(const client::KuduSchema& query_schema,
                   const std::vector<client::KuduPredicate*>& preds,
                   gscoped_ptr<Scanner>* scanner);

  simple_spinlock lock_;
  std::tr1::shared_ptr<client::KuduClient> client_;
  std::tr1::shared_ptr<client::KuduSession> session_;
  std::tr1::shared_ptr<client::KuduTable> client_table_;
  const std::string master_address_;
  const std::string table_name_;
  const MonoDelta timeout_;
  const int batch_max_;
  const std::vector<std::string> tablet_splits_;
  int batch_size_;

  // Semaphore which restricts us to one batch at a time.
  Semaphore semaphore_;
};

} //namespace kudu
#endif
