#ifndef K_PG_H_
#define K_PG_H_

namespace K {
  mPosition pgPos;
  mSafety pgSafety;
  double pgTargetBasePos = 0;
  string pgSideAPR = "";
  class PG: public Klass {
    protected:
      void load() {
        json k = ((DB*)memory)->load(uiTXT::TargetBasePosition);
        if (k.size()) {
          k = k.at(0);
          pgTargetBasePos = k.value("tbp", 0.0);
          pgSideAPR = k.value("sideAPR", "");
        }
        stringstream ss;
        ss << setprecision(8) << fixed << pgTargetBasePos;
        FN::log("DB", string("loaded TBP = ") + ss.str() + " " + gw->base);
        k = ((DB*)memory)->load(uiTXT::Position);
        if (k.size()) {
          for (json::reverse_iterator it = k.rbegin(); it != k.rend(); ++it)
            pgProfit.push_back(mProfit(
              (*it)["baseValue"].get<double>(),
              (*it)["quoteValue"].get<double>(),
              (*it)["time"].get<unsigned long>()
            ));
          FN::log("DB", string("loaded ") + to_string(pgProfit.size()) + " historical Profits");
        }
      };
      void waitData() {
        gw->evDataWallet = [&](mWallet k) {
          if (((CF*)config)->argDebugEvents) FN::log("DEBUG", string("EV PG evDataWallet mWallet ") + ((json)k).dump());
          calcWallet(k);
        };
        ((EV*)events)->ogOrder = [&](mOrder k) {
          if (((CF*)config)->argDebugEvents) FN::log("DEBUG", string("EV PG ogOrder mOrder ") + ((json)k).dump());
          calcWalletAfterOrder(k);
          FN::screen_refresh();
        };
        ((EV*)events)->mgTargetPosition = [&]() {
          if (((CF*)config)->argDebugEvents) FN::log("DEBUG", "EV PG mgTargetPosition");
          calcTargetBasePos();
        };
      };
      void waitUser() {
        ((UI*)client)->welcome(uiTXT::Position, &helloPosition);
        ((UI*)client)->welcome(uiTXT::TradeSafetyValue, &helloSafety);
        ((UI*)client)->welcome(uiTXT::TargetBasePosition, &helloTargetBasePos);
      };
    public:
      void calcSafety() {
        if (empty() or !mgFairValue) return;
        mSafety safety = nextSafety();
        pgMutex.lock();
        if (pgSafety.buyPing == -1
          or safety.combined != pgSafety.combined
          or safety.buyPing != pgSafety.buyPing
          or safety.sellPong != pgSafety.sellPong
        ) {
          pgSafety = safety;
          pgMutex.unlock();
          ((UI*)client)->send(uiTXT::TradeSafetyValue, safety);
        } else pgMutex.unlock();
      };
      void calcTargetBasePos() {
        static string pgSideAPR_ = "!=";
        if (empty()) { FN::logWar("QE", "Unable to calculate TBP, missing market data."); return; }
        pgMutex.lock();
        double value = pgPos.value;
        pgMutex.unlock();
        double targetBasePosition = qp.autoPositionMode == mAutoPositionMode::Manual
          ? (qp.percentageValues
            ? qp.targetBasePositionPercentage * value / 1e+2
            : qp.targetBasePosition)
          : ((1 + mgTargetPos) / 2) * value;
        if (pgTargetBasePos and abs(pgTargetBasePos - targetBasePosition) < 1e-4 and pgSideAPR_ == pgSideAPR) return;
        pgTargetBasePos = targetBasePosition;
        pgSideAPR_ = pgSideAPR;
        ((EV*)events)->pgTargetBasePosition();
        json k = {{"tbp", pgTargetBasePos}, {"sideAPR", pgSideAPR}};
        ((UI*)client)->send(uiTXT::TargetBasePosition, k, true);
        ((DB*)memory)->insert(uiTXT::TargetBasePosition, k);
        stringstream ss;
        ss << (int)(pgTargetBasePos / value * 1e+2) << "% = " << setprecision(8) << fixed << pgTargetBasePos;
        FN::log("TBP", ss.str() + " " + gw->base);
      };
      void addTrade(mTrade k) {
        mTrade k_(k.price, k.quantity, k.time);
        if (k.side == mSide::Bid) pgBuys[k.price] = k_;
        else pgSells[k.price] = k_;
      };
      bool empty() {
        lock_guard<mutex> lock(pgMutex);
        return !pgPos.value;
      };
    private:
      vector<mProfit> pgProfit;
      map<double, mTrade> pgBuys;
      map<double, mTrade> pgSells;
      function<json()> helloPosition = []() {
        lock_guard<mutex> lock(pgMutex);
        return (json){ pgPos };
      };
      function<json()> helloSafety = []() {
        lock_guard<mutex> lock(pgMutex);
        return (json){ pgSafety };
      };
      function<json()> helloTargetBasePos = []() {
        return (json){{{"tbp", pgTargetBasePos}, {"sideAPR", pgSideAPR}}};
      };
      mSafety nextSafety() {
        pgMutex.lock();
        double value          = pgPos.value,
               baseAmount     = pgPos.baseAmount,
               baseHeldAmount = pgPos.baseHeldAmount;
        pgMutex.unlock();
        double buySize = qp.percentageValues
          ? qp.buySizePercentage * value / 100
          : qp.buySize;
        double sellSize = qp.percentageValues
          ? qp.sellSizePercentage * value / 100
          : qp.sellSize;
        double totalBasePosition = baseAmount + baseHeldAmount;
        if (qp.buySizeMax and qp.aggressivePositionRebalancing != mAPR::Off)
          buySize = fmax(buySize, pgTargetBasePos - totalBasePosition);
        if (qp.sellSizeMax and qp.aggressivePositionRebalancing != mAPR::Off)
          sellSize = fmax(sellSize, totalBasePosition - pgTargetBasePos);
        double widthPong = qp.widthPercentage
          ? qp.widthPongPercentage * mgFairValue / 100
          : qp.widthPong;
        map<double, mTrade> tradesBuy;
        map<double, mTrade> tradesSell;
        for (vector<mTrade>::iterator it = ((OG*)orders)->tradesHistory.begin(); it != ((OG*)orders)->tradesHistory.end(); ++it)
          if (it->side == mSide::Bid)
            tradesBuy[it->price] = *it;
          else tradesSell[it->price] = *it;
        double buyPing = 0;
        double sellPong = 0;
        double buyQty = 0;
        double sellQty = 0;
        if (qp.pongAt == mPongAt::ShortPingFair
          or qp.pongAt == mPongAt::ShortPingAggressive
        ) {
          matchBestPing(&tradesBuy, &buyPing, &buyQty, sellSize, widthPong, true);
          matchBestPing(&tradesSell, &sellPong, &sellQty, buySize, widthPong);
          if (!buyQty) matchFirstPing(&tradesBuy, &buyPing, &buyQty, sellSize, widthPong*-1, true);
          if (!sellQty) matchFirstPing(&tradesSell, &sellPong, &sellQty, buySize, widthPong*-1);
        } else if (qp.pongAt == mPongAt::LongPingFair
          or qp.pongAt == mPongAt::LongPingAggressive
        ) {
          matchLastPing(&tradesBuy, &buyPing, &buyQty, sellSize, widthPong);
          matchLastPing(&tradesSell, &sellPong, &sellQty, buySize, widthPong, true);
        }
        if (buyQty) buyPing /= buyQty;
        if (sellQty) sellPong /= sellQty;
        clean();
        double sumBuys = sum(&pgBuys);
        double sumSells = sum(&pgSells);
        return mSafety(
          sumBuys / buySize,
          sumSells / sellSize,
          (sumBuys + sumSells) / (buySize + sellSize),
          buyPing,
          sellPong
        );
      };
      void matchFirstPing(map<double, mTrade>* trades, double* ping, double* qty, double qtyMax, double width, bool reverse = false) {
        matchPing(((QP*)params)->matchPings(), true, true, trades, ping, qty, qtyMax, width, reverse);
      };
      void matchBestPing(map<double, mTrade>* trades, double* ping, double* qty, double qtyMax, double width, bool reverse = false) {
        matchPing(((QP*)params)->matchPings(), true, false, trades, ping, qty, qtyMax, width, reverse);
      };
      void matchLastPing(map<double, mTrade>* trades, double* ping, double* qty, double qtyMax, double width, bool reverse = false) {
        matchPing(((QP*)params)->matchPings(), false, true, trades, ping, qty, qtyMax, width, reverse);
      };
      void matchPing(bool matchPings, bool near, bool far, map<double, mTrade>* trades, double* ping, double* qty, double qtyMax, double width, bool reverse = false) {
        int dir = width > 0 ? 1 : -1;
        if (reverse) for (map<double, mTrade>::reverse_iterator it = trades->rbegin(); it != trades->rend(); ++it) {
          if (matchPing(matchPings, near, far, ping, width, qty, qtyMax, dir * mgFairValue, dir * it->second.price, it->second.quantity, it->second.price, it->second.Kqty, reverse))
            break;
        } else for (map<double, mTrade>::iterator it = trades->begin(); it != trades->end(); ++it)
          if (matchPing(matchPings, near, far, ping, width, qty, qtyMax, dir * mgFairValue, dir * it->second.price, it->second.quantity, it->second.price, it->second.Kqty, reverse))
            break;
      };
      bool matchPing(bool matchPings, bool near, bool far, double *ping, double width, double* qty, double qtyMax, double fv, double price, double qtyTrade, double priceTrade, double KqtyTrade, bool reverse) {
        if (reverse) { fv *= -1; price *= -1; width *= -1; }
        if (*qty < qtyMax
          and (far ? fv > price : true)
          and (near ? (reverse ? fv - width : fv + width) < price : true)
          and (!matchPings or KqtyTrade < qtyTrade)
        ) matchPing(ping, qty, qtyMax, qtyTrade, priceTrade);
        return *qty >= qtyMax;
      };
      void matchPing(double* ping, double* qty, double qtyMax, double qtyTrade, double priceTrade) {
        double qty_ = fmin(qtyMax - *qty, qtyTrade);
        *ping += priceTrade * qty_;
        *qty += qty_;
      };
      void clean() {
        if (pgBuys.size()) expire(&pgBuys);
        if (pgSells.size()) expire(&pgSells);
        skip();
      };
      void expire(map<double, mTrade>* k) {
        unsigned long now = FN::T();
        for (map<double, mTrade>::iterator it = k->begin(); it != k->end();)
          if (it->second.time + qp.tradeRateSeconds * 1e+3 > now) ++it;
          else it = k->erase(it);
      };
      void skip() {
        while (pgBuys.size() and pgSells.size()) {
          mTrade buy = pgBuys.rbegin()->second;
          mTrade sell = pgSells.begin()->second;
          if (sell.price < buy.price) break;
          double buyQty = buy.quantity;
          buy.quantity = buyQty - sell.quantity;
          sell.quantity = sell.quantity - buyQty;
          if (buy.quantity < gw->minSize)
            pgBuys.erase(--pgBuys.rbegin().base());
          if (sell.quantity < gw->minSize)
            pgSells.erase(pgSells.begin());
        }
      };
      double sum(map<double, mTrade>* k) {
        double sum = 0;
        for (map<double, mTrade>::iterator it = k->begin(); it != k->end(); ++it)
          sum += it->second.quantity;
        return sum;
      };
      void calcWallet(mWallet k) {
        static unsigned long profitT_21s = 0;
        static mutex walletMutex,
                     profitMutex;
        static map<string, mWallet> pgWallet;
        walletMutex.lock();
        if (k.currency!="") pgWallet[k.currency] = k;
        if (!mgFairValue or pgWallet.find(gw->base) == pgWallet.end() or pgWallet.find(gw->quote) == pgWallet.end()) {
          walletMutex.unlock();
          return;
        }
        mWallet baseWallet = pgWallet[gw->base];
        mWallet quoteWallet = pgWallet[gw->quote];
        walletMutex.unlock();
        double baseValue = baseWallet.amount + quoteWallet.amount / mgFairValue + baseWallet.held + quoteWallet.held / mgFairValue;
        double quoteValue = baseWallet.amount * mgFairValue + quoteWallet.amount + baseWallet.held * mgFairValue + quoteWallet.held;
        unsigned long now = FN::T();
        mProfit profit(baseValue, quoteValue, now);
        if (profitT_21s+21e+3 < FN::T()) {
          profitT_21s = FN::T();
          ((DB*)memory)->insert(uiTXT::Position, profit, false, "NULL", now - qp.profitHourInterval * 36e+5);
        }
        profitMutex.lock();
        pgProfit.push_back(profit);
        for (vector<mProfit>::iterator it = pgProfit.begin(); it != pgProfit.end();)
          if (it->time + (qp.profitHourInterval * 36e+5) > now) ++it;
          else it = pgProfit.erase(it);
        mPosition pos(
          baseWallet.amount,
          quoteWallet.amount,
          baseWallet.held,
          quoteWallet.held,
          baseValue,
          quoteValue,
          ((baseValue - pgProfit.begin()->baseValue) / baseValue) * 1e+2,
          ((quoteValue - pgProfit.begin()->quoteValue) / quoteValue) * 1e+2,
          mPair(gw->base, gw->quote),
          gw->exchange
        );
        profitMutex.unlock();
        bool eq = true;
        if (!empty()) {
          pgMutex.lock();
          eq = abs(pos.value - pgPos.value) < 2e-6;
          if(eq
            and abs(pos.quoteValue - pgPos.quoteValue) < 2e-2
            and abs(pos.baseAmount - pgPos.baseAmount) < 2e-6
            and abs(pos.quoteAmount - pgPos.quoteAmount) < 2e-2
            and abs(pos.baseHeldAmount - pgPos.baseHeldAmount) < 2e-6
            and abs(pos.quoteHeldAmount - pgPos.quoteHeldAmount) < 2e-2
            and abs(pos.profitBase - pgPos.profitBase) < 2e-2
            and abs(pos.profitQuote - pgPos.profitQuote) < 2e-2
          ) { pgMutex.unlock(); return; }
        } else pgMutex.lock();
        pgPos = pos;
        pgMutex.unlock();
        if (!eq) calcTargetBasePos();
        ((UI*)client)->send(uiTXT::Position, pos, true);
      };
      void calcWalletAfterOrder(mOrder k) {
        if (empty()) return;
        double heldAmount = 0;
        pgMutex.lock();
        double amount = k.side == mSide::Ask
          ? pgPos.baseAmount + pgPos.baseHeldAmount
          : pgPos.quoteAmount + pgPos.quoteHeldAmount;
        pgMutex.unlock();
        ogMutex.lock();
        for (map<string, mOrder>::iterator it = allOrders.begin(); it != allOrders.end(); ++it) {
          if (it->second.side != k.side) continue;
          double held = it->second.quantity * (it->second.side == mSide::Bid ? it->second.price : 1);
          if (amount >= held) {
            amount -= held;
            heldAmount += held;
          }
        }
        ogMutex.unlock();
        calcWallet(mWallet(amount, heldAmount, k.side == mSide::Ask
          ? k.pair.base : k.pair.quote
        ));
      };
  };
}

#endif
