#include "datashard_impl.h"

namespace NKikimr {
namespace NDataShard {

using namespace NTabletFlatExecutor;

class TDataShard::TTxChangeExchangeHandshake: public TTransactionBase<TDataShard> {
    using Schema = TDataShard::Schema;
    static constexpr ui64 MaxBatchSize = 1000;

public:
    explicit TTxChangeExchangeHandshake(TDataShard* self)
        : TTransactionBase(self)
    {
    }

    TTxType GetTxType() const override {
        return TXTYPE_CHANGE_EXCHANGE_HANDSHAKE;
    }

    bool Precharge(NIceDb::TNiceDb& db) {
        bool ok = true;

        ui32 i = 0;
        for (const auto& [_, req] : Self->PendingChangeExchangeHandshakes) {
            if (++i > MaxBatchSize) {
                break;
            }

            auto it = Self->InChangeSenders.find(req.GetOrigin());
            if (it == Self->InChangeSenders.end()) {
                ok = ok && db.Table<Schema::ChangeSenders>().Key(req.GetOrigin()).Precharge();
            }
        }

        return ok;
    }

    bool Process(NIceDb::TNiceDb& db, const TActorContext& ctx) {
        Statuses.reserve(Min(Self->PendingChangeExchangeHandshakes.size(), MaxBatchSize));

        ui32 i = 0;
        while (!Self->PendingChangeExchangeHandshakes.empty()) {
            if (++i > MaxBatchSize) {
                break;
            }

            const auto& [sender, req] = Self->PendingChangeExchangeHandshakes.front();
            auto& [_, resp] = Statuses.emplace_back(sender, MakeHolder<TEvChangeExchange::TEvStatus>());
            Y_VERIFY(ExecuteHandshake(db, req, resp->Record));
            Self->PendingChangeExchangeHandshakes.pop_front();
        }

        Self->ChangeExchangeHandshakeExecuted();
        if (Self->PendingChangeExchangeHandshakes.size() < MaxBatchSize) {
            Self->StartCollectingChangeExchangeHandshakes(ctx);
        } else {
            Self->RunChangeExchangeHandshakeTx();
        }

        return true;
    }

    bool ExecuteHandshake(NIceDb::TNiceDb& db, const NKikimrChangeExchange::TEvHandshake& req, NKikimrChangeExchange::TEvStatus& resp) {
        if (Self->State != TShardState::Ready) {
            resp.SetStatus(NKikimrChangeExchange::TEvStatus::STATUS_REJECT);
            resp.SetReason(NKikimrChangeExchange::TEvStatus::REASON_WRONG_STATE);
            return true;
        }

        auto it = Self->InChangeSenders.find(req.GetOrigin());
        if (it == Self->InChangeSenders.end()) {
            auto rowset = db.Table<Schema::ChangeSenders>().Key(req.GetOrigin()).Select();
            if (!rowset.IsReady()) {
                return false;
            }

            if (rowset.IsValid()) {
                const auto generation = rowset.GetValue<Schema::ChangeSenders::Generation>();
                const auto lastRecordOrder = rowset.GetValueOrDefault<Schema::ChangeSenders::LastRecordOrder>(0);
                it = Self->InChangeSenders.emplace(req.GetOrigin(), TInChangeSender(generation, lastRecordOrder)).first;
            } else {
                it = Self->InChangeSenders.emplace(req.GetOrigin(), TInChangeSender(req.GetGeneration())).first;
            }
        }

        if (it->second.Generation > req.GetGeneration()) {
            resp.SetStatus(NKikimrChangeExchange::TEvStatus::STATUS_REJECT);
            resp.SetReason(NKikimrChangeExchange::TEvStatus::REASON_STALE_ORIGIN);
        } else {
            it->second.Generation = req.GetGeneration();
            resp.SetStatus(NKikimrChangeExchange::TEvStatus::STATUS_OK);
            resp.SetLastRecordOrder(it->second.LastRecordOrder);
        }

        return true;
    }

    bool Execute(TTransactionContext& txc, const TActorContext& ctx) override {
        NIceDb::TNiceDb db(txc.DB);

        if (!Precharge(db)) {
            return false;
        }

        return Process(db, ctx);
    }

    void Complete(const TActorContext& ctx) override {
        for (auto& [sender, status] : Statuses) {
            ctx.Send(sender, status.Release());
        }
    }

private:
    TVector<std::pair<TActorId, THolder<TEvChangeExchange::TEvStatus>>> Statuses;

}; // TTxChangeExchangeHandshake

class TDataShard::TTxApplyChangeRecords: public TTransactionBase<TDataShard> {
    static NTable::ERowOp GetRowOperation(const NKikimrChangeExchange::TChangeRecord::TDataChange& record) {
        switch (record.GetRowOperationCase()) {
            case NKikimrChangeExchange::TChangeRecord::TDataChange::kUpsert:
                return NTable::ERowOp::Upsert;
            case NKikimrChangeExchange::TChangeRecord::TDataChange::kErase:
                return NTable::ERowOp::Erase;
            case NKikimrChangeExchange::TChangeRecord::TDataChange::kReset:
                return NTable::ERowOp::Reset;
            default:
                return NTable::ERowOp::Absent;
        }
    }

    static auto& GetValue(const NKikimrChangeExchange::TChangeRecord::TDataChange& record) {
        switch (record.GetRowOperationCase()) {
            case NKikimrChangeExchange::TChangeRecord::TDataChange::kUpsert:
                return record.GetUpsert();
            case NKikimrChangeExchange::TChangeRecord::TDataChange::kReset:
                return record.GetReset();
            default:
                Y_FAIL_S("Unexpected row operation: " << static_cast<ui32>(record.GetRowOperationCase()));
        }
    }

    void AddRecordStatus(const TActorContext& ctx, ui64 order, NKikimrChangeExchange::TEvStatus::EStatus status,
            NKikimrChangeExchange::TEvStatus::EReason reason = NKikimrChangeExchange::TEvStatus::REASON_NONE,
            const TString& error = {})
    {
        auto& recordStatus = *Status->Record.AddRecordStatuses();
        recordStatus.SetOrder(order);
        recordStatus.SetStatus(status);
        recordStatus.SetReason(reason);

        if (error) {
            LOG_CRIT_S(ctx, NKikimrServices::TX_DATASHARD, "Cannot apply change record"
                << ": error# " << error
                << ", at tablet# " << Self->TabletID());
        }

        if (status == NKikimrChangeExchange::TEvStatus::STATUS_REJECT) {
            Status->Record.SetStatus(status);
        }

        if (Status->Record.GetStatus() != NKikimrChangeExchange::TEvStatus::STATUS_REJECT) {
            Status->Record.SetStatus(NKikimrChangeExchange::TEvStatus::STATUS_OK);
            Status->Record.SetLastRecordOrder(order);
        }
    }

    bool ProcessRecord(const NKikimrChangeExchange::TChangeRecord& record, TTransactionContext& txc, const TActorContext& ctx) {
        Key.clear();
        Value.clear();

        if (record.GetKindCase() != NKikimrChangeExchange::TChangeRecord::kAsyncIndex) {
            AddRecordStatus(ctx, record.GetOrder(), NKikimrChangeExchange::TEvStatus::STATUS_REJECT,
                NKikimrChangeExchange::TEvStatus::REASON_UNEXPECTED_KIND,
                TStringBuilder() << "Unexpected kind: " << static_cast<ui32>(record.GetKindCase()));
            return false;
        }

        const auto& userTables = Self->GetUserTables();
        auto it = userTables.find(record.GetLocalPathId());
        if (it == userTables.end()) {
            AddRecordStatus(ctx, record.GetOrder(), NKikimrChangeExchange::TEvStatus::STATUS_REJECT,
                NKikimrChangeExchange::TEvStatus::REASON_SCHEME_ERROR,
                TStringBuilder() << "Unknown table with id: " << record.GetLocalPathId());
            return false;
        }

        const auto& tableInfo = *it->second;
        const auto& asyncIndex = record.GetAsyncIndex();
        const auto& serializedKey = asyncIndex.GetKey();

        if (serializedKey.TagsSize() != tableInfo.KeyColumnIds.size()) {
            AddRecordStatus(ctx, record.GetOrder(), NKikimrChangeExchange::TEvStatus::STATUS_REJECT,
                NKikimrChangeExchange::TEvStatus::REASON_SCHEME_ERROR,
                TStringBuilder() << "Key column count mismatch"
                    << ": got " << serializedKey.TagsSize()
                    << ", expected " << tableInfo.KeyColumnIds.size());
            return false;
        }

        for (size_t i = 0; i < tableInfo.KeyColumnIds.size(); ++i) {
            if (serializedKey.GetTags(i) != tableInfo.KeyColumnIds.at(i)) {
                AddRecordStatus(ctx, record.GetOrder(), NKikimrChangeExchange::TEvStatus::STATUS_REJECT,
                    NKikimrChangeExchange::TEvStatus::REASON_SCHEME_ERROR,
                    TStringBuilder() << "Key column schema mismatch at position: " << i);
                return false;
            }
        }

        if (!TSerializedCellVec::TryParse(serializedKey.GetData(), KeyCells)) {
            AddRecordStatus(ctx, record.GetOrder(), NKikimrChangeExchange::TEvStatus::STATUS_REJECT,
                NKikimrChangeExchange::TEvStatus::REASON_SCHEME_ERROR, "Cannot parse key");
            return false;
        }

        if (KeyCells.GetCells().size() != tableInfo.KeyColumnTypes.size()) {
            AddRecordStatus(ctx, record.GetOrder(), NKikimrChangeExchange::TEvStatus::STATUS_REJECT,
                NKikimrChangeExchange::TEvStatus::REASON_SCHEME_ERROR,
                TStringBuilder() << "Cell count doesn't match row scheme"
                    << ": got " << KeyCells.GetCells().size()
                    << ", expected " << tableInfo.KeyColumnTypes.size());
            return false;
        }

        ui64 keyBytes = 0;
        for (size_t i = 0; i < tableInfo.KeyColumnTypes.size(); ++i) {
            const auto type = tableInfo.KeyColumnTypes.at(i);
            const auto& cell = KeyCells.GetCells().at(i);

            if (type.GetTypeId() == NScheme::NTypeIds::Uint8 && !cell.IsNull() && cell.AsValue<ui8>() > 127) {
                AddRecordStatus(ctx, record.GetOrder(), NKikimrChangeExchange::TEvStatus::STATUS_REJECT,
                    NKikimrChangeExchange::TEvStatus::REASON_SCHEME_ERROR,
                    "Keys with Uint8 column values >127 are currently prohibited");
                return false;
            }

            keyBytes += cell.Size();
            Key.emplace_back(cell.AsRef(), type);
        }

        if (keyBytes > NLimits::MaxWriteKeySize) {
            AddRecordStatus(ctx, record.GetOrder(), NKikimrChangeExchange::TEvStatus::STATUS_REJECT,
                NKikimrChangeExchange::TEvStatus::REASON_SCHEME_ERROR,
                TStringBuilder() << "Key is too big"
                    << ": actual " << keyBytes << " bytes"
                    << ", limit " << NLimits::MaxWriteKeySize << " bytes");
            return false;
        }

        const NTable::ERowOp rop = GetRowOperation(asyncIndex);
        switch (rop) {
            case NTable::ERowOp::Upsert:
            case NTable::ERowOp::Reset: {
                const auto& serializedValue = GetValue(asyncIndex);

                if (!TSerializedCellVec::TryParse(serializedValue.GetData(), ValueCells)) {
                    AddRecordStatus(ctx, record.GetOrder(), NKikimrChangeExchange::TEvStatus::STATUS_REJECT,
                        NKikimrChangeExchange::TEvStatus::REASON_SCHEME_ERROR, "Cannot parse value");
                    return false;
                }

                if (serializedValue.TagsSize() != ValueCells.GetCells().size()) {
                    AddRecordStatus(ctx, record.GetOrder(), NKikimrChangeExchange::TEvStatus::STATUS_REJECT,
                        NKikimrChangeExchange::TEvStatus::REASON_SCHEME_ERROR,
                        TStringBuilder() << "Cell count doesn't match row scheme"
                            << ": got " << ValueCells.GetCells().size()
                            << ", expected " << serializedValue.TagsSize());
                    return false;
                }

                for (size_t i = 0; i < ValueCells.GetCells().size(); ++i) {
                    const auto tag = serializedValue.GetTags(i);

                    const auto* column = tableInfo.Columns.FindPtr(tag);
                    if (!column) {
                        AddRecordStatus(ctx, record.GetOrder(), NKikimrChangeExchange::TEvStatus::STATUS_REJECT,
                            NKikimrChangeExchange::TEvStatus::REASON_SCHEME_ERROR,
                            TStringBuilder() << "Missing column with id: " << tag);
                        return false;
                    }

                    const auto& cell = ValueCells.GetCells().at(i);
                    if (cell.Size() > NLimits::MaxWriteValueSize) {
                        AddRecordStatus(ctx, record.GetOrder(), NKikimrChangeExchange::TEvStatus::STATUS_REJECT,
                            NKikimrChangeExchange::TEvStatus::REASON_SCHEME_ERROR,
                            TStringBuilder() << "Value cell is too big"
                                << ": actual " << cell.Size() << " bytes"
                                << ", limit " << NLimits::MaxWriteValueSize << " bytes");
                        return false;
                    }

                    Value.emplace_back(tag, NTable::ECellOp::Set, TRawTypeValue(cell.AsRef(), column->Type));
                }

                break;
            }

            case NTable::ERowOp::Erase:
                break;

            case NTable::ERowOp::Absent:
                AddRecordStatus(ctx, record.GetOrder(), NKikimrChangeExchange::TEvStatus::STATUS_REJECT,
                    NKikimrChangeExchange::TEvStatus::REASON_UNEXPECTED_ROW_OPERATION, "Row operation is absent");
                return false;
        }

        txc.DB.Update(tableInfo.LocalTid, rop, Key, Value, TRowVersion(record.GetStep(), record.GetTxId()));
        Self->GetConflictsCache().GetTableCache(tableInfo.LocalTid).RemoveUncommittedWrites(KeyCells.GetCells(), txc.DB);
        tableInfo.Stats.UpdateTime = TAppData::TimeProvider->Now();
        AddRecordStatus(ctx, record.GetOrder(), NKikimrChangeExchange::TEvStatus::STATUS_OK);

        return true;
    }

public:
    explicit TTxApplyChangeRecords(TDataShard* self, TEvChangeExchange::TEvApplyRecords::TPtr ev)
        : TTransactionBase(self)
        , Ev(std::move(ev))
        , Status(new TEvChangeExchange::TEvStatus)
    {
    }

    TTxType GetTxType() const override {
        return TXTYPE_APPLY_CHANGE_RECORDS;
    }

    bool Execute(TTransactionContext& txc, const TActorContext& ctx) override {
        if (Self->State != TShardState::Ready) {
            Status->Record.SetStatus(NKikimrChangeExchange::TEvStatus::STATUS_REJECT);
            Status->Record.SetReason(NKikimrChangeExchange::TEvStatus::REASON_WRONG_STATE);
            return true;
        }

        const auto& msg = Ev->Get()->Record;

        auto it = Self->InChangeSenders.find(msg.GetOrigin());
        if (it == Self->InChangeSenders.end()) {
            Status->Record.SetStatus(NKikimrChangeExchange::TEvStatus::STATUS_REJECT);
            Status->Record.SetReason(NKikimrChangeExchange::TEvStatus::REASON_UNKNOWN_ORIGIN);
            return true;
        } else if (it->second.Generation > msg.GetGeneration()) {
            Status->Record.SetStatus(NKikimrChangeExchange::TEvStatus::STATUS_REJECT);
            Status->Record.SetReason(NKikimrChangeExchange::TEvStatus::REASON_STALE_ORIGIN);
            return true;
        } else if (it->second.Generation != msg.GetGeneration()) {
            Status->Record.SetStatus(NKikimrChangeExchange::TEvStatus::STATUS_REJECT);
            Status->Record.SetReason(NKikimrChangeExchange::TEvStatus::REASON_UNKNOWN_ORIGIN);
            return true;
        }

        auto completeEdge = TRowVersion::Min();
        for (const auto& record : msg.GetRecords()) {
            if (record.GetOrder() <= it->second.LastRecordOrder) {
                AddRecordStatus(ctx, record.GetOrder(), NKikimrChangeExchange::TEvStatus::STATUS_REJECT,
                    NKikimrChangeExchange::TEvStatus::REASON_ORDER_VIOLATION,
                    TStringBuilder() << "Last record order: " << it->second.LastRecordOrder);
                break;
            }
            if (ProcessRecord(record, txc, ctx)) {
                completeEdge = Max(completeEdge, TRowVersion(record.GetStep(), record.GetTxId()));
            } else {
                break;
            }
        }

        NIceDb::TNiceDb db(txc.DB);
        db.Table<Schema::ChangeSenders>().Key(it->first).Update(
            NIceDb::TUpdate<Schema::ChangeSenders::Generation>(it->second.Generation),
            NIceDb::TUpdate<Schema::ChangeSenders::LastSeenAt>(ctx.Now().GetValue())
        );

        if (Status->Record.HasLastRecordOrder()) {
            it->second.LastRecordOrder = Status->Record.GetLastRecordOrder();
            db.Table<Schema::ChangeSenders>().Key(it->first).Update(
                NIceDb::TUpdate<Schema::ChangeSenders::LastRecordOrder>(it->second.LastRecordOrder)
            );
        }

        if (completeEdge) {
            Self->PromoteCompleteEdge(completeEdge, txc);
        }

        return true;
    }

    void Complete(const TActorContext& ctx) override {
        Y_VERIFY(Status);
        ctx.Send(Ev->Sender, Status.Release());
    }

private:
    TEvChangeExchange::TEvApplyRecords::TPtr Ev;
    THolder<TEvChangeExchange::TEvStatus> Status;

    TSerializedCellVec KeyCells;
    TSerializedCellVec ValueCells;

    TVector<TRawTypeValue> Key;
    TVector<NTable::TUpdateOp> Value;

}; // TTxApplyChangeRecords

void TDataShard::StartCollectingChangeExchangeHandshakes(const TActorContext& ctx) {
    if (!ChangeExchangeHandshakesCollecting) {
        ChangeExchangeHandshakesCollecting = true;
        ctx.Schedule(TDuration::MilliSeconds(25), new TEvPrivate::TEvChangeExchangeExecuteHandshakes);
    }
}

void TDataShard::Handle(TEvPrivate::TEvChangeExchangeExecuteHandshakes::TPtr&, const TActorContext&) {
    ChangeExchangeHandshakesCollecting = false;
    RunChangeExchangeHandshakeTx();
}

void TDataShard::RunChangeExchangeHandshakeTx() {
    if (!ChangeExchangeHandshakeTxScheduled && !PendingChangeExchangeHandshakes.empty()) {
        ChangeExchangeHandshakeTxScheduled = true;
        EnqueueExecute(new TTxChangeExchangeHandshake(this));
    }
}

void TDataShard::ChangeExchangeHandshakeExecuted() {
    ChangeExchangeHandshakeTxScheduled = false;
}

void TDataShard::Handle(TEvChangeExchange::TEvHandshake::TPtr& ev, const TActorContext& ctx) {
    PendingChangeExchangeHandshakes.emplace_back(ev->Sender, std::move(ev->Get()->Record));
    StartCollectingChangeExchangeHandshakes(ctx);
}

void TDataShard::Handle(TEvChangeExchange::TEvApplyRecords::TPtr& ev, const TActorContext& ctx) {
    LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD, "Handle TEvChangeExchange::TEvApplyRecords"
        << ": origin# " << ev->Get()->Record.GetOrigin()
        << ", generation# " << ev->Get()->Record.GetGeneration()
        << ", at tablet# " << TabletID());
    Execute(new TTxApplyChangeRecords(this, ev), ctx);
}

} // NDataShard
} // NKikimr
