-- MySQL 8.4 schema version 1, derived from the r42 empty PostgreSQL schema 54.

-- bf-step table TransactionLocks
CREATE TABLE TransactionLocks (LockKey VARCHAR(32) NOT NULL PRIMARY KEY) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin;

-- bf-step table AccountCredentials
CREATE TABLE `AccountCredentials` (
  `AccountId` BIGINT NOT NULL COMMENT '拥有该凭据的账号，一账号初版至多一组活动凭据。',
  `Algorithm` TEXT NOT NULL COMMENT '密码派生算法标识；现有兼容值为 pbkdf2_sha256。',
  `Salt` LONGBLOB NOT NULL COMMENT '密码派生随机盐的原始字节。',
  `Digest` LONGBLOB NOT NULL COMMENT '密码派生结果的原始字节。',
  `Iterations` INT NOT NULL COMMENT '密码派生迭代次数，必须大于零；现有 Python 基线为 600000。',
  `UpdatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '凭据最后创建或轮换的数据库时间。',
  CONSTRAINT `CK_AccountCredentialsAlgorithm` CHECK (`Algorithm` = 'pbkdf2_sha256'),
  CONSTRAINT `CK_AccountCredentialsDigestLength` CHECK (octet_length(`Digest`) = 32),
  CONSTRAINT `CK_AccountCredentialsIterationsRange` CHECK (`Iterations` >= 100000 AND `Iterations` <= 10000000),
  CONSTRAINT `CK_AccountCredentialsSaltLength` CHECK (octet_length(`Salt`) = 16),
  PRIMARY KEY (`AccountId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='保存兼容启动器密码派生所需的算法名、盐、摘要和迭代次数；不保存明文密码。';

-- bf-step table AccountDeviceLogins
CREATE TABLE `AccountDeviceLogins` (
  `LoginId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '登录记录编号。',
  `AccountId` BIGINT NOT NULL COMMENT '账号编号。',
  `MachineCode` VARCHAR(69) NOT NULL COMMENT '设备摘要。',
  `LoggedInAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '成功登录时间。',
  `RegionId` INT NOT NULL COMMENT '目标大区编号。',
  CONSTRAINT `CK_AccountDeviceLoginsRegion` CHECK (`RegionId` > 0),
  PRIMARY KEY (`LoginId`),
  KEY `IX_AccountDeviceLoginsAccount` (`AccountId`, `LoggedInAt` DESC)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='每次成功密码登录的持久记录。';

-- bf-step table AccountDevices
CREATE TABLE `AccountDevices` (
  `AccountId` BIGINT NOT NULL COMMENT '账号编号。',
  `MachineCode` VARCHAR(69) NOT NULL COMMENT '设备摘要。',
  `FirstSeenAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '首次成功登录时间。',
  `LastSeenAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '最近成功登录时间。',
  `LoginCount` BIGINT NOT NULL DEFAULT 1 COMMENT '累计成功登录次数。',
  CONSTRAINT `CK_AccountDevicesLoginCount` CHECK (`LoginCount` > 0),
  PRIMARY KEY (`AccountId`, `MachineCode`),
  KEY `IX_AccountDevicesMachine` (`MachineCode`, `AccountId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='账号与设备的成功登录关联。';

-- bf-step table AccountEmails
CREATE TABLE `AccountEmails` (
  `AccountId` BIGINT NOT NULL COMMENT '现有稳定账号主键；绑定不创建替代账号。',
  `EmailAddress` TEXT NOT NULL COMMENT '展示用邮箱地址，域名统一小写。',
  `EmailKey` VARCHAR(254) NOT NULL COMMENT '按服务器策略生成的邮箱唯一比较键，不折叠点或加号别名。',
  `VerifiedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '邮箱所有权验证成功的数据库时间。',
  CONSTRAINT `CK_AccountEmailsAddress` CHECK (octet_length(`EmailAddress`) >= 3 AND octet_length(`EmailAddress`) <= 254),
  CONSTRAINT `CK_AccountEmailsKeyLength` CHECK (octet_length(`EmailKey`) >= 3 AND octet_length(`EmailKey`) <= 254),
  PRIMARY KEY (`AccountId`),
  UNIQUE KEY `UX_AccountEmailsKey` (`EmailKey`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='账号已验证邮箱；邮箱绑定不改变账号主键或EOS身份。';

-- bf-step table Accounts
CREATE TABLE `Accounts` (
  `AccountId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '账号强类型主键；迁移时可显式保留旧 uid，但业务代码不得等同于 player_id。',
  `LoginName` TEXT NOT NULL COMMENT '用于展示和审计的原始登录名。',
  `LoginKey` VARCHAR(64) NOT NULL COMMENT '按冻结的 Unicode/大小写规则生成的唯一登录键。',
  `AccountStatus` TEXT NOT NULL DEFAULT ('active') COMMENT '账号状态：active、disabled 或 locked。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '账号建立时间；旧数据使用可验证来源时间或迁移切点并在报告中标明。',
  `AccountOrigin` TEXT NOT NULL DEFAULT ('native') COMMENT '身份来源：native 为新服务创建，legacy_launcher 为旧启动器账号，legacy_local 为无启动器凭据的旧本地身份。',
  `ActiveMachineCode` VARCHAR(69) COMMENT '最近一次认领在线身份成功后绑定的设备摘要。',
  CONSTRAINT `CK_AccountsLegacyLocalDisabled` CHECK (`AccountOrigin` <> 'legacy_local' OR `AccountStatus` = 'disabled'),
  CONSTRAINT `CK_AccountsLoginKeyFormat` CHECK ((`AccountOrigin` IN ('native', 'legacy_launcher')) AND REGEXP_LIKE(`LoginKey`,'^[a-z0-9_]{3,24}$','c') OR `AccountOrigin` = 'legacy_local' AND char_length(`LoginKey`) >= 1 AND char_length(`LoginKey`) <= 64 AND NOT REGEXP_LIKE(`LoginKey`,'[[:cntrl:]]','c')),
  CONSTRAINT `CK_AccountsLoginKeyMatches` CHECK (`AccountOrigin` = 'legacy_local' OR `LoginKey` = lower(`LoginName`)),
  CONSTRAINT `CK_AccountsLoginNameFormat` CHECK ((`AccountOrigin` IN ('native', 'legacy_launcher')) AND REGEXP_LIKE(`LoginName`,'^[A-Za-z0-9_]{3,24}$','c') OR `AccountOrigin` = 'legacy_local' AND char_length(`LoginName`) >= 1 AND char_length(`LoginName`) <= 32 AND NOT REGEXP_LIKE(`LoginName`,'[[:cntrl:]]','c')),
  CONSTRAINT `CK_AccountsOrigin` CHECK (`AccountOrigin` IN ('native', 'legacy_launcher', 'legacy_local')),
  CONSTRAINT `CK_AccountsStatus` CHECK (`AccountStatus` IN ('active', 'disabled', 'locked')),
  PRIMARY KEY (`AccountId`),
  UNIQUE KEY `UX_AccountsLoginKey` (`LoginKey`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='登录账号主体；不混放玩家昵称、钱包余额或装备状态。';

-- bf-step table AdminAuditEvents
CREATE TABLE `AdminAuditEvents` (
  `AdminAuditEventId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '管理审计事件 UUID。',
  `Actor` TEXT NOT NULL COMMENT '经过认证的操作员或自动任务稳定标识。',
  `ActionCode` VARCHAR(32) NOT NULL COMMENT '补偿、冻结或配置操作等稳定动作代码。',
  `BusinessId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '本次管理动作唯一业务 UUID，用于拒绝重复执行。',
  `TargetSnapshot` JSON NOT NULL COMMENT '动作目标和前后状态的脱敏 JSON 对象。',
  `Reason` TEXT NOT NULL COMMENT '非空人工或自动处置原因，不得包含密码、票据和连接秘密。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '审计事件与管理事务提交时间。',
  CONSTRAINT `CK_AdminAuditActionNonempty` CHECK (char_length(`ActionCode`) > 0),
  CONSTRAINT `CK_AdminAuditActorNonempty` CHECK (char_length(`Actor`) > 0),
  CONSTRAINT `CK_AdminAuditReasonNonempty` CHECK (char_length(`Reason`) > 0),
  CONSTRAINT `CK_AdminAuditTargetObject` CHECK (LOWER(JSON_TYPE(`TargetSnapshot`)) = 'object'),
  PRIMARY KEY (`AdminAuditEventId`),
  UNIQUE KEY `UX_AdminAuditEventsBusiness` (`BusinessId`),
  KEY `IX_AdminAuditEventsCreated` (`CreatedAt`, `AdminAuditEventId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='不可变管理、修正和补偿审计；不能用普通日志替代。';

-- bf-step table BubbleDraws
CREATE TABLE `BubbleDraws` (
  `DrawId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '新服务生成的递增抽取编号。',
  `PlayerId` BIGINT NOT NULL COMMENT '发起抽取并拥有待领取结果的玩家。',
  `OperationRequestId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin COMMENT '在线抽取的稳定操作请求；仅导入旧结果时可为 NULL。',
  `NeedleItemId` BIGINT NOT NULL COMMENT '事务中实际消耗的针类物品模板。',
  `Positions` JSON NOT NULL COMMENT '固定格位数组；恢复和领取不得重新随机。',
  `RewardSnapshot` JSON NOT NULL COMMENT '固定奖励数组及来源信息；不从当前目录重建。',
  `DrawStatus` TEXT NOT NULL DEFAULT ('pending') COMMENT '状态：pending、claimed 或 cancelled。',
  `RulesVersionId` BIGINT NOT NULL COMMENT '随机和奖励采用的已发布规则版本。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '抽取事务提交时间。',
  `ClaimedAt` DATETIME(6) COMMENT '奖励成功领取时间；未领取或取消时为 NULL。',
  `LegacyDrawId` BIGINT COMMENT '旧 SQLite bubble_draws.id；新抽取为 NULL。',
  `ClaimedChoice` SMALLINT COMMENT '二选一奖励下标，只允许 0 或 1；未领取为空。',
  `ClaimedPosition` SMALLINT COMMENT '实际领取的客户端泡泡格位，只允许 0 至 4；未领取为空。',
  `LegacyImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin COMMENT '旧泡泡抽取结果的来源导入批次；不得因此重新抽取或重复发奖。',
  CONSTRAINT `CK_BubbleDrawsClaimSelection` CHECK (`DrawStatus` = 'claimed' AND `ClaimedChoice` >= 0 AND `ClaimedChoice` <= 1 AND `ClaimedPosition` >= 0 AND `ClaimedPosition` <= 4 OR `DrawStatus` <> 'claimed' AND `ClaimedChoice` IS NULL AND `ClaimedPosition` IS NULL),
  CONSTRAINT `CK_BubbleDrawsClaimedTime` CHECK (`DrawStatus` = 'claimed' AND `ClaimedAt` IS NOT NULL AND `ClaimedAt` >= `CreatedAt` OR `DrawStatus` <> 'claimed' AND `ClaimedAt` IS NULL),
  CONSTRAINT `CK_BubbleDrawsPositionsArray` CHECK (LOWER(JSON_TYPE(`Positions`)) = 'array'),
  CONSTRAINT `CK_BubbleDrawsRewardsArray` CHECK (LOWER(JSON_TYPE(`RewardSnapshot`)) = 'array'),
  CONSTRAINT `CK_BubbleDrawsStatus` CHECK (`DrawStatus` IN ('pending', 'claimed', 'cancelled')),
  PRIMARY KEY (`DrawId`),
  UNIQUE KEY `UX_BubbleDrawsLegacy` (`LegacyDrawId`),
  UNIQUE KEY `UX_BubbleDrawsOperation` (`OperationRequestId`),
  KEY `IX_BubbleDrawsPlayerCreated` (`PlayerId`, `CreatedAt`, `DrawId`),
  `ConditionalUqBubbleDrawsPlayerPendingPlayerId` BIGINT GENERATED ALWAYS AS (CASE WHEN (`DrawStatus` = 'pending') THEN `PlayerId` ELSE NULL END) STORED,
  UNIQUE KEY `UX_BubbleDrawsPlayerPending` (`ConditionalUqBubbleDrawsPlayerPendingPlayerId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='泡泡抽取的固定位置、奖励和领取状态；待领取结果迁移后保持不变且只能领取一次。';

-- bf-step table ChatPenaltyHistory
CREATE TABLE `ChatPenaltyHistory` (
  `PenaltyId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '管理记录的稳定身份。',
  `PlayerId` BIGINT NOT NULL COMMENT '被处罚玩家。',
  `Reason` TEXT NOT NULL COMMENT '经管理员确认的聊天处罚理由。',
  `RecordedBy` TEXT NOT NULL COMMENT '写入该事实的服务端管理员标识。',
  `RecordedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '处罚事实记录时间。',
  `RevokedAt` DATETIME(6) COMMENT '纠正或撤销该记录的时间；普通禁言到期不会填充。',
  `RevocationReason` TEXT COMMENT '撤销该记录的明确理由。',
  CONSTRAINT `CK_ChatPenaltyHistoryOperator` CHECK (char_length(`RecordedBy`) >= 1 AND char_length(`RecordedBy`) <= 64),
  CONSTRAINT `CK_ChatPenaltyHistoryReason` CHECK (char_length(`Reason`) >= 1 AND char_length(`Reason`) <= 256),
  CONSTRAINT `CK_ChatPenaltyHistoryRevocation` CHECK (`RevokedAt` IS NULL AND `RevocationReason` IS NULL OR `RevokedAt` IS NOT NULL AND `RevokedAt` >= `RecordedAt` AND `RevocationReason` IS NOT NULL AND char_length(`RevocationReason`) >= 1 AND char_length(`RevocationReason`) <= 256),
  PRIMARY KEY (`PenaltyId`),
  KEY `IX_ChatPenaltyHistoryValid` (`PlayerId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='服务端聊天处罚历史；Clean聊天只读取未撤销记录，不使用关键词或客户端自报标志。';

-- bf-step table ClanActivityGrants
CREATE TABLE `ClanActivityGrants` (
  `GrantId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '稳定流水编号。',
  `ClanId` BIGINT NOT NULL COMMENT '取得积分时冻结的普通战队。',
  `PlayerId` BIGINT NOT NULL COMMENT '本次贡献积分的玩家。',
  `SourceKind` VARCHAR(191) NOT NULL COMMENT '积分来源：login出勤或match正常结算。',
  `SourceId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '每日登录奖励业务UUID或原始比赛UUID。',
  `Points` INT NOT NULL COMMENT '原客户端活跃度说明中已核实的单次积分。',
  `EarnedAt` DATETIME(6) NOT NULL COMMENT '源业务发生UTC时间，查询按上海自然周或月份划分。',
  `PolicyVersion` INT NOT NULL DEFAULT 1 COMMENT '冻结的积分规则版本；版本1使用原UI的出勤及2人、4人战队赛说明。',
  CONSTRAINT `CK_ClanActivityOrdinary` CHECK (`ClanId` <> 99),
  CONSTRAINT `CK_ClanActivityPoints` CHECK (`Points` >= 1 AND `Points` <= 60),
  CONSTRAINT `CK_ClanActivityPolicy` CHECK (`PolicyVersion` = 1),
  CONSTRAINT `CK_ClanActivitySource` CHECK (`SourceKind` IN ('login', 'match')),
  PRIMARY KEY (`GrantId`),
  UNIQUE KEY `UX_ClanActivitySource` (`SourceKind`, `SourceId`, `PlayerId`),
  KEY `IX_ClanActivityPeriod` (`ClanId`, `EarnedAt`, `PlayerId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='战队活跃积分不可变来源流水；出勤冻结当时成员关系，比赛冻结开局归属，事务回滚及重试不会重复加分。';

-- bf-step table ClanApplications
CREATE TABLE `ClanApplications` (
  `ClanApplicationId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '申请历史递增主键。',
  `ApplicantPlayerId` BIGINT NOT NULL COMMENT '提交加入申请的玩家。',
  `ClanId` BIGINT NOT NULL COMMENT '申请加入的目标战队。',
  `ApplicationStatus` VARCHAR(191) NOT NULL DEFAULT 'pending' COMMENT '申请状态：pending、accepted、rejected 或 cancelled。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '申请建立时间。',
  `ResolvedAt` DATETIME(6) COMMENT '接受、拒绝或取消时间；待处理时为 NULL。',
  `LegacyImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin COMMENT '旧待审批申请的来源导入批次。',
  CONSTRAINT `CK_ClanApplicationsResolution` CHECK (`ApplicationStatus` = 'pending' AND `ResolvedAt` IS NULL OR `ApplicationStatus` <> 'pending' AND `ResolvedAt` IS NOT NULL),
  CONSTRAINT `CK_ClanApplicationsStatus` CHECK (`ApplicationStatus` IN ('pending', 'accepted', 'rejected', 'cancelled')),
  PRIMARY KEY (`ClanApplicationId`),
  KEY `IX_ClanApplicationsClanPending` (`ClanId`, `ApplicationStatus`, `CreatedAt`, `ClanApplicationId`),
  `ConditionalUqClanApplicationsPlayerPendingApplicantPlayerId` BIGINT GENERATED ALWAYS AS (CASE WHEN (`ApplicationStatus` = 'pending') THEN `ApplicantPlayerId` ELSE NULL END) STORED,
  UNIQUE KEY `UX_ClanApplicationsPlayerPending` (`ConditionalUqClanApplicationsPlayerPendingApplicantPlayerId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='战队加入申请历史；每名玩家同时至多一项 pending，终态记录保留用于审计。';

-- bf-step table ClanAuditEvents
CREATE TABLE `ClanAuditEvents` (
  `AuditEventId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '新服务生成的递增审计游标。',
  `ClanId` BIGINT COMMENT '事件发生时的战队编号；战队已删除或旧数据缺失时可为 NULL。',
  `ActorPlayerId` BIGINT COMMENT '事件发生时的操作者玩家编号；系统或不可映射旧记录为 NULL。',
  `ActorSnapshot` TEXT NOT NULL COMMENT '操作者显示快照；系统操作使用稳定系统标识。',
  `ActionCode` VARCHAR(32) NOT NULL COMMENT '稳定管理动作代码，例如 create、member_change 或 disband。',
  `TargetPlayerId` BIGINT COMMENT '动作涉及的可选目标玩家编号。',
  `TargetSnapshot` JSON NOT NULL DEFAULT ('{}') COMMENT '目标及当时角色等结构化快照；必须是 JSON 对象。',
  `Detail` JSON NOT NULL DEFAULT ('{}') COMMENT '动作参数和迁移来源等结构化详情；不保存密码、票据或聊天正文。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '审计事件创建时间。',
  `LegacyAuditId` BIGINT COMMENT '旧 SQLite clan_audit.id；新事件为 NULL。',
  `LegacyImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin COMMENT '旧战队审计事件的来源导入批次。',
  CONSTRAINT `CK_ClanAuditActionNonempty` CHECK (char_length(`ActionCode`) > 0),
  CONSTRAINT `CK_ClanAuditActorSnapshotNonempty` CHECK (char_length(`ActorSnapshot`) > 0),
  CONSTRAINT `CK_ClanAuditDetailObject` CHECK (LOWER(JSON_TYPE(`Detail`)) = 'object'),
  CONSTRAINT `CK_ClanAuditTargetObject` CHECK (LOWER(JSON_TYPE(`TargetSnapshot`)) = 'object'),
  PRIMARY KEY (`AuditEventId`),
  UNIQUE KEY `UX_ClanAuditEventsLegacy` (`LegacyAuditId`),
  KEY `IX_ClanAuditEventsClanCursor` (`ClanId`, `CreatedAt`, `AuditEventId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='不可变战队管理审计；即使战队或账号退出活动数据，事件仍保留原始快照。';

-- bf-step table ClanDisbandRequests
CREATE TABLE `ClanDisbandRequests` (
  `DisbandRequestId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '解散请求递增主键。',
  `ClanId` BIGINT NOT NULL COMMENT '计划解散的战队。',
  `RequestedByPlayerId` BIGINT NOT NULL COMMENT '提交请求时经事务确认的会长玩家。',
  `RequestStatus` VARCHAR(191) NOT NULL DEFAULT 'pending' COMMENT '状态：pending、cancelled 或 executed。',
  `ExecuteAfter` DATETIME(6) NOT NULL COMMENT '允许执行解散的 UTC 时间，不用零时间表示无请求。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '请求建立时间。',
  `ResolvedAt` DATETIME(6) COMMENT '取消或执行时间；待处理时为 NULL。',
  `LegacyImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin COMMENT '旧待执行解散请求的来源导入批次。',
  CONSTRAINT `CK_ClanDisbandExecuteOrder` CHECK (`ExecuteAfter` >= `CreatedAt`),
  CONSTRAINT `CK_ClanDisbandResolution` CHECK (`RequestStatus` = 'pending' AND `ResolvedAt` IS NULL OR `RequestStatus` <> 'pending' AND `ResolvedAt` IS NOT NULL),
  CONSTRAINT `CK_ClanDisbandStatus` CHECK (`RequestStatus` IN ('pending', 'cancelled', 'executed')),
  PRIMARY KEY (`DisbandRequestId`),
  KEY `IX_ClanDisbandExecutePending` (`RequestStatus`, `ExecuteAfter`, `DisbandRequestId`),
  `ConditionalUqClanDisbandClanPendingClanId` BIGINT GENERATED ALWAYS AS (CASE WHEN (`RequestStatus` = 'pending') THEN `ClanId` ELSE NULL END) STORED,
  UNIQUE KEY `UX_ClanDisbandClanPending` (`ConditionalUqClanDisbandClanPendingClanId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='战队解散请求历史；新服务按明确任务执行，不在普通进程重启时隐式删除战队。';

-- bf-step table ClanLeadershipTransfers
CREATE TABLE `ClanLeadershipTransfers` (
  `TransferId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '转让请求递增主键。',
  `ClanId` BIGINT NOT NULL COMMENT '发生会长转让的战队。',
  `SourcePlayerId` BIGINT NOT NULL COMMENT '发起转让的当前会长。',
  `TargetPlayerId` BIGINT NOT NULL COMMENT '必须在同战队且在线确认的目标成员。',
  `TransferStatus` VARCHAR(191) NOT NULL DEFAULT 'pending' COMMENT '状态：pending、accepted、declined、expired 或 cancelled。',
  `ExpiresAt` DATETIME(6) NOT NULL COMMENT '目标必须响应的 UTC 截止时间；旧行为为发起后 120 秒。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '请求建立时间。',
  `ResolvedAt` DATETIME(6) COMMENT '请求进入终态的时间；待处理时为 NULL。',
  `LegacyImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin COMMENT '旧会长转让请求的来源导入批次。',
  `LegacyAccepted` BOOLEAN COMMENT '旧流程中接收方是否已确认；在线新流程为 NULL，避免把旧 accepted 位误解为已完成会长变更。',
  CONSTRAINT `CK_ClanTransfersDifferentPlayers` CHECK (`SourcePlayerId` <> `TargetPlayerId`),
  CONSTRAINT `CK_ClanTransfersExpiryOrder` CHECK (`ExpiresAt` > `CreatedAt`),
  CONSTRAINT `CK_ClanTransfersResolution` CHECK (`TransferStatus` = 'pending' AND `ResolvedAt` IS NULL OR `TransferStatus` <> 'pending' AND `ResolvedAt` IS NOT NULL),
  CONSTRAINT `CK_ClanTransfersStatus` CHECK (`TransferStatus` IN ('pending', 'accepted', 'declined', 'expired', 'cancelled')),
  PRIMARY KEY (`TransferId`),
  KEY `IX_ClanTransfersTargetPending` (`TargetPlayerId`, `TransferStatus`, `ExpiresAt`),
  `ConditionalUqClanTransfersClanPendingClanId` BIGINT GENERATED ALWAYS AS (CASE WHEN (`TransferStatus` = 'pending') THEN `ClanId` ELSE NULL END) STORED,
  UNIQUE KEY `UX_ClanTransfersClanPending` (`ConditionalUqClanTransfersClanPendingClanId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='限时会长转让请求历史；接受事务同时更新战队会长和双方成员角色。';

-- bf-step table ClanMatchSides
CREATE TABLE `ClanMatchSides` (
  `MatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '同一开局事务内建立的持久化对局编号。',
  `TeamCode` INT NOT NULL COMMENT '原生一队或二队，和开局成员队伍一致。',
  `ClanId` BIGINT NOT NULL COMMENT '该队开局时的战队编号，后续退队不修改。',
  CONSTRAINT `CK_ClanMatchSidesOrdinary` CHECK (`ClanId` <> 99),
  CONSTRAINT `CK_ClanMatchSidesTeam` CHECK (`TeamCode` IN (1, 2)),
  PRIMARY KEY (`MatchId`, `TeamCode`),
  UNIQUE KEY `UX_ClanMatchSidesClan` (`MatchId`, `ClanId`),
  KEY `IX_ClanMatchSidesClan` (`ClanId`, `MatchId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='开局冻结的家族频道对阵；双方人数相等且每队来自不同普通战队才登记，历史不按当前成员重建。';

-- bf-step table ClanMemberships
CREATE TABLE `ClanMemberships` (
  `PlayerId` BIGINT NOT NULL COMMENT '成员玩家，也是每玩家唯一当前归属主键。',
  `ClanId` BIGINT NOT NULL COMMENT '成员所属战队。',
  `RoleCode` VARCHAR(191) NOT NULL COMMENT '角色代码：member、master 或 deputy；协议数字只在边界映射。',
  `JoinedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '成员加入战队的数据库时间。',
  `LegacyImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin COMMENT '旧成员关系的来源导入批次；用于逐玩家归属对账。',
  CONSTRAINT `CK_ClanMembershipsRole` CHECK (`RoleCode` IN ('member', 'master', 'deputy')),
  PRIMARY KEY (`PlayerId`),
  UNIQUE KEY `UX_ClanMembershipsClanPlayer` (`ClanId`, `PlayerId`),
  KEY `IX_ClanMembershipsClanRole` (`ClanId`, `RoleCode`, `PlayerId`),
  `ConditionalUqClanMembershipsOneDeputyClanId` BIGINT GENERATED ALWAYS AS (CASE WHEN (`RoleCode` = 'deputy') THEN `ClanId` ELSE NULL END) STORED,
  UNIQUE KEY `UX_ClanMembershipsOneDeputy` (`ConditionalUqClanMembershipsOneDeputyClanId`),
  `ConditionalUqClanMembershipsOneMasterClanId` BIGINT GENERATED ALWAYS AS (CASE WHEN (`RoleCode` = 'master') THEN `ClanId` ELSE NULL END) STORED,
  UNIQUE KEY `UX_ClanMembershipsOneMaster` (`ConditionalUqClanMembershipsOneMasterClanId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='玩家当前战队归属和权限角色；初版每名玩家最多加入一个战队。';

-- bf-step table ClanMessages
CREATE TABLE `ClanMessages` (
  `MessageId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '新服务生成的递增战队消息游标。',
  `ClanId` BIGINT NOT NULL COMMENT '消息所属战队。',
  `SenderPlayerId` BIGINT COMMENT '可选发送玩家编号；导入时无法可靠映射则为 NULL。',
  `SenderSnapshot` TEXT NOT NULL COMMENT '发送时昵称快照。',
  `MessageBody` TEXT NOT NULL COMMENT '非空聊天正文；应用层校验 UTF-16 边界和控制字符。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '消息提交时间。',
  `LegacyMessageId` BIGINT COMMENT '旧 SQLite clan_chat.id；新消息为 NULL。',
  `LegacyImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin COMMENT '旧战队聊天的来源导入批次；正文不得进入普通转换日志。',
  CONSTRAINT `CK_ClanMessagesBodyLength` CHECK (char_length(`MessageBody`) >= 1 AND char_length(`MessageBody`) <= 256),
  CONSTRAINT `CK_ClanMessagesSenderSnapshotNonempty` CHECK (char_length(`SenderSnapshot`) > 0),
  PRIMARY KEY (`MessageId`),
  UNIQUE KEY `UX_ClanMessagesLegacy` (`LegacyMessageId`),
  KEY `IX_ClanMessagesClanCursor` (`ClanId`, `CreatedAt`, `MessageId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='战队聊天历史；保存发送时昵称快照，正文按隐私数据处理。';

-- bf-step table ClanPosts
CREATE TABLE `ClanPosts` (
  `PostId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '新服务生成的递增留言游标。',
  `ClanId` BIGINT NOT NULL COMMENT '留言所属战队。',
  `AuthorPlayerId` BIGINT COMMENT '可选作者玩家编号；无法可靠映射的旧审计数据为 NULL。',
  `AuthorSnapshot` TEXT NOT NULL COMMENT '发布时作者昵称快照，不从后来修改的资料重建历史。',
  `PostBody` TEXT NOT NULL COMMENT '非空留言正文；应用层校验 UTF-16 边界和控制字符。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '留言发布时间。',
  `LegacyPostId` BIGINT COMMENT '旧 SQLite clan_posts.id；新留言为 NULL。',
  `LegacyImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin COMMENT '旧战队留言的来源导入批次；与 legacy_post_id 共同防止重复导入。',
  CONSTRAINT `CK_ClanPostsAuthorSnapshotNonempty` CHECK (char_length(`AuthorSnapshot`) > 0),
  CONSTRAINT `CK_ClanPostsBodyLength` CHECK (char_length(`PostBody`) >= 1 AND char_length(`PostBody`) <= 256),
  PRIMARY KEY (`PostId`),
  UNIQUE KEY `UX_ClanPostsLegacy` (`LegacyPostId`),
  KEY `IX_ClanPostsClanCursor` (`ClanId`, `CreatedAt`, `PostId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='战队留言板当前与迁移历史；作者显示快照允许在账号生命周期变化后保持审计上下文。';

-- bf-step table ClanStarterGrants
CREATE TABLE `ClanStarterGrants` (
  `PlayerId` BIGINT NOT NULL COMMENT '已经领取或从旧标记导入的玩家，一名玩家仅一行。',
  `ItemId` BIGINT NOT NULL COMMENT '实际发放的战队创建卡物品模板。',
  `GrantedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '在线发放提交时间或旧标记迁移切点。',
  `InventoryTransactionId` BIGINT COMMENT '在线发放对应的库存流水；仅导入旧标记且无法恢复流水时为 NULL。',
  `ImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin COMMENT '旧去重标记的来源导入批次；在线新发放为 NULL。',
  PRIMARY KEY (`PlayerId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='战队创建卡初始发放去重标记；导入旧标记后不得再次赠送。';

-- bf-step table Clans
CREATE TABLE `Clans` (
  `ClanId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '战队强类型主键；迁移时可显式保留旧 clans.id。',
  `ClanName` TEXT NOT NULL COMMENT '战队展示名称；客户端另限制 UTF-16 代码单元和控制字符。',
  `ClanNameKey` VARCHAR(191) NOT NULL COMMENT '按冻结规范化规则生成的唯一名称键，不能用数据库 lower 临时代替。',
  `LeaderPlayerId` BIGINT COMMENT '普通战队当前会长，必须同时存在 master 成员；系统训练营为 NULL。',
  `Introduction` TEXT NOT NULL DEFAULT ('') COMMENT '战队简介；允许空字符串。',
  `Notice` TEXT NOT NULL DEFAULT ('') COMMENT '战队公告；允许空字符串。',
  `MarkCode` INT NOT NULL DEFAULT 0 COMMENT '旧客户端使用的徽章编号，当前已验证范围为 0 至 255。',
  `Capacity` INT NOT NULL COMMENT '成员容量；创建卡决定的已知值含 10、30，旧默认值含 50。',
  `ClanStatus` VARCHAR(191) NOT NULL DEFAULT 'active' COMMENT '战队状态：active、pending_disband 或 disbanded。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '战队创建时间；旧 Unix 秒按 UTC 转换。',
  `LegacyImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin COMMENT '旧战队主体的来源导入批次；原生新建战队为 NULL。',
  `ClanKind` VARCHAR(191) NOT NULL DEFAULT 'standard' COMMENT '战队类型：standard 为玩家管理战队；training 为无会长、不可管理的系统训练营。',
  `RecruitmentMode` SMALLINT NOT NULL DEFAULT 1 COMMENT '原版招募模式：0 停止招募、1 申请审批、2 直接加入。旧战队保留原有审批流程。',
  CONSTRAINT `CK_ClansCapacityRange` CHECK (`Capacity` >= 1 AND `Capacity` <= 200),
  CONSTRAINT `CK_ClansIntroductionLength` CHECK (char_length(`Introduction`) <= 100),
  CONSTRAINT `CK_ClansKind` CHECK (`ClanKind` IN ('standard', 'training')),
  CONSTRAINT `CK_ClansKindLeader` CHECK (`ClanKind` = 'standard' AND `LeaderPlayerId` IS NOT NULL OR `ClanKind` = 'training' AND `LeaderPlayerId` IS NULL),
  CONSTRAINT `CK_ClansMarkRange` CHECK (`MarkCode` >= 0 AND `MarkCode` <= 255),
  CONSTRAINT `CK_ClansNameKeyNonempty` CHECK (char_length(`ClanNameKey`) > 0),
  CONSTRAINT `CK_ClansNameLength` CHECK (char_length(`ClanName`) >= 2 AND char_length(`ClanName`) <= 12),
  CONSTRAINT `CK_ClansNoticeLength` CHECK (char_length(`Notice`) <= 256),
  CONSTRAINT `CK_ClansRecruitmentMode` CHECK (`RecruitmentMode` IN (0, 1, 2)),
  CONSTRAINT `CK_ClansStatus` CHECK (`ClanStatus` IN ('active', 'pending_disband', 'disbanded')),
  PRIMARY KEY (`ClanId`),
  UNIQUE KEY `UX_ClansNameKey` (`ClanNameKey`),
  KEY `IX_ClansStatusId` (`ClanStatus`, `ClanId`),
  `ConditionalUqClansSingleTrainingClanKind` VARCHAR(191) GENERATED ALWAYS AS (CASE WHEN (`ClanKind` = 'training') THEN `ClanKind` ELSE NULL END) STORED,
  UNIQUE KEY `UX_ClansSingleTraining` (`ConditionalUqClansSingleTrainingClanKind`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='战队主体及可编辑展示资料；会长与成员表一致性由锁定本行的战队事务维护。';

-- bf-step table ConfigurationVersions
CREATE TABLE `ConfigurationVersions` (
  `ConfigurationVersionId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '配置发布版本主键。',
  `ConfigurationKind` VARCHAR(191) NOT NULL COMMENT '配置类别，例如 catalog、progression_rules 或 reward_rules。',
  `SourceName` TEXT NOT NULL COMMENT '可审计的来源名称，不保存个人机器绝对路径。',
  `ContentDigest` VARBINARY(64) NOT NULL COMMENT '规范化发布内容的安全摘要。',
  `Payload` JSON NOT NULL COMMENT '可复现配置载荷；关系完整性字段仍使用普通列和外键。',
  `ReleaseStatus` TEXT NOT NULL DEFAULT ('draft') COMMENT '发布状态：draft、published 或 retired。',
  `PublishedAt` DATETIME(6) COMMENT '首次发布时间；草稿为 NULL，已发布或退役版本保留原时间。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '配置版本入库时间。',
  CONSTRAINT `CK_ConfigurationVersionsDigestNonempty` CHECK (octet_length(`ContentDigest`) > 0),
  CONSTRAINT `CK_ConfigurationVersionsKindNonempty` CHECK (char_length(`ConfigurationKind`) > 0),
  CONSTRAINT `CK_ConfigurationVersionsPublishState` CHECK (`ReleaseStatus` = 'draft' AND `PublishedAt` IS NULL OR (`ReleaseStatus` IN ('published', 'retired')) AND `PublishedAt` IS NOT NULL),
  CONSTRAINT `CK_ConfigurationVersionsSourceNonempty` CHECK (char_length(`SourceName`) > 0),
  CONSTRAINT `CK_ConfigurationVersionsStatus` CHECK (`ReleaseStatus` IN ('draft', 'published', 'retired')),
  PRIMARY KEY (`ConfigurationVersionId`),
  UNIQUE KEY `UX_ConfigurationVersionsKindDigest` (`ConfigurationKind`, `ContentDigest`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='保存可复现的商品、奖励和玩法规则发布版本及内容摘要。';

-- bf-step table CoupleDailyGrants
CREATE TABLE `CoupleDailyGrants` (
  `PlayerId` BIGINT NOT NULL COMMENT '领取玩家。',
  `RewardDay` DATE NOT NULL COMMENT '有效对局开局日期，固定UTC+8；延迟恢复和换情侣不会重置。',
  `MatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '触发赠品的有效共同对局。',
  `CoupleId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '触发时冻结的情侣身份。',
  `GrantedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '事务发放时间。',
  PRIMARY KEY (`PlayerId`, `RewardDay`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='草莓戒指每日赠品防重账本；按玩家和UTC+8日期限领。';

-- bf-step table CoupleMembers
CREATE TABLE `CoupleMembers` (
  `PlayerId` BIGINT NOT NULL COMMENT '稳定玩家身份，改名不改变关系。',
  `CoupleId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '共同的情侣关系，解除关系时删除双方成员。',
  PRIMARY KEY (`PlayerId`),
  KEY `IX_CoupleMembersCouple` (`CoupleId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='每个玩家最多属于一个情侣关系，提交时关系必须恰好两人。';

-- bf-step table Couples
CREATE TABLE `Couples` (
  `CoupleId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL DEFAULT (UUID()) COMMENT '关系与库存审计的稳定业务编号。',
  `RingItemId` BIGINT NOT NULL COMMENT '原版邀请戒指对应的正式戒指模板。',
  `FormedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '双方同意并成功扣除邀请戒指的时间。',
  CONSTRAINT `CK_CouplesRing` CHECK (`RingItemId` IN (100605, 100985, 101035, 101396, 101561)),
  PRIMARY KEY (`CoupleId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='原版情侣关系；正式戒指和开始日期以此处为准，邀请不扣道具。';

-- bf-step table CouponRedemptions
CREATE TABLE `CouponRedemptions` (
  `CouponId` BIGINT NOT NULL COMMENT '已核销的发行记录。',
  `AccountId` BIGINT NOT NULL COMMENT '兑换账号；重建角色不重置资格。',
  `PlayerId` BIGINT NOT NULL COMMENT '实际收到资产的角色。',
  `BusinessId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '关联所有奖励流水的稳定业务 UUID。',
  `CurrencyAmount` BIGINT NOT NULL COMMENT '原生回包所需的已提交货币数量。',
  `ItemCount` BIGINT NOT NULL COMMENT '原生回包所需的已提交物品数量。',
  `RedeemedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '成功核销的事务时间。',
  CONSTRAINT `CK_CouponRedemptionsRewards` CHECK (`CurrencyAmount` >= 0 AND `ItemCount` >= 0),
  PRIMARY KEY (`CouponId`, `AccountId`),
  UNIQUE KEY `UX_CouponRedemptionsBusiness` (`BusinessId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='兑换券按账号仅可成功核销一次的耐久回执，与钱包和库存流水同时提交。';

-- bf-step table Coupons
CREATE TABLE `Coupons` (
  `CouponId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '发行记录主键。',
  `CodeHash` VARCHAR(64) NOT NULL COMMENT 'ASCII 大写兑换码摘要；不保存可直接使用的原文。',
  `Label` TEXT NOT NULL COMMENT '发行批次说明。',
  `StockId` BIGINT COMMENT '可选的库存奖励规格，在兑换事务内校验已发布目录。',
  `CurrencyCode` VARCHAR(191) COMMENT '可选的奖励币种，不同钱包不得互换。',
  `CurrencyAmount` BIGINT NOT NULL DEFAULT 0 COMMENT '每次成功兑换实际发放的整数金额。',
  `StartsAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '允许兑换的开始时间。',
  `ExpiresAt` DATETIME(6) NOT NULL COMMENT '允许兑换的截止时间，不含边界。',
  `MaximumUses` BIGINT NOT NULL DEFAULT 1 COMMENT '全券核销总上限。',
  `UsedCount` BIGINT NOT NULL DEFAULT 0 COMMENT '事务内锁定并递增的成功核销次数。',
  `Enabled` BOOLEAN NOT NULL DEFAULT true COMMENT '运营显式启停开关；停用不删除核销历史。',
  CONSTRAINT `CK_CouponsCurrency` CHECK (`CurrencyCode` IS NULL AND `CurrencyAmount` = 0 OR `CurrencyCode` IS NOT NULL AND (`CurrencyCode` IN ('lucci', 'bubble_cash', 'treasure_bubble')) AND `CurrencyAmount` >= 1 AND `CurrencyAmount` <= 2147483647),
  CONSTRAINT `CK_CouponsHash` CHECK (REGEXP_LIKE(`CodeHash`,'^[0-9a-f]{64}$','c')),
  CONSTRAINT `CK_CouponsLabel` CHECK (char_length(`Label`) >= 1 AND char_length(`Label`) <= 128),
  CONSTRAINT `CK_CouponsReward` CHECK (`StockId` IS NOT NULL OR `CurrencyAmount` > 0),
  CONSTRAINT `CK_CouponsTime` CHECK (`ExpiresAt` > `StartsAt`),
  CONSTRAINT `CK_CouponsUses` CHECK (`MaximumUses` > 0 AND `UsedCount` >= 0 AND `UsedCount` <= `MaximumUses`),
  PRIMARY KEY (`CouponId`),
  UNIQUE KEY `UX_CouponsCode` (`CodeHash`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='显式发行的兑换券目录，只保存规范化兑换码的 SHA-256 摘要。';

-- bf-step table DailyLoginRewards
CREATE TABLE `DailyLoginRewards` (
  `PlayerId` BIGINT NOT NULL COMMENT '领取玩家标识。',
  `RewardDate` DATE NOT NULL COMMENT '北京时间自然日，与玩家组成唯一领取键。',
  `BusinessId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '两笔钱包流水共享的幂等业务标识。',
  `BubbleCashAmount` BIGINT NOT NULL DEFAULT 99999 COMMENT '本次发放泡泡点数量。',
  `LucciAmount` BIGINT NOT NULL DEFAULT 99999 COMMENT '本次发放金币数量。',
  `ClaimedAt` DATETIME(6) NOT NULL COMMENT '服务端确认本次登录奖励的 UTC 时间。',
  CONSTRAINT `CK_DailyLoginRewardsCash` CHECK (`BubbleCashAmount` = 99999),
  CONSTRAINT `CK_DailyLoginRewardsLucci` CHECK (`LucciAmount` = 99999),
  PRIMARY KEY (`PlayerId`, `RewardDate`),
  UNIQUE KEY `UX_DailyLoginRewardsBusiness` (`BusinessId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='每日游戏登录奖励；reward_date 固定使用 UTC+08:00 自然日，重连和重启不重复领取。';

-- bf-step table DataImportBatches
CREATE TABLE `DataImportBatches` (
  `ImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '应用生成的稳定导入批次 UUID，用于期初流水和异常报告关联。',
  `SourceKind` VARCHAR(191) NOT NULL COMMENT '来源类型，例如 sqlite_snapshot 或 client_catalog_export。',
  `SourceDigest` VARBINARY(64) NOT NULL COMMENT '来源文件或一致性快照的安全摘要，禁止保存秘密内容。',
  `ImportVersion` VARCHAR(191) NOT NULL COMMENT '导入器及转换规则的版本标识。',
  `ImportStatus` TEXT NOT NULL COMMENT '批次状态：running、succeeded 或 failed。',
  `StartedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '导入开始的数据库时间。',
  `CompletedAt` DATETIME(6) COMMENT '导入结束时间；运行中为 NULL。',
  `Report` JSON NOT NULL DEFAULT ('{}') COMMENT '结构化汇总与异常计数，不保存密码、票据或未脱敏私人正文。',
  CONSTRAINT `CK_DataImportBatchesDigestNonempty` CHECK (octet_length(`SourceDigest`) > 0),
  CONSTRAINT `CK_DataImportBatchesReportObject` CHECK (LOWER(JSON_TYPE(`Report`)) = 'object'),
  CONSTRAINT `CK_DataImportBatchesSourceKindNonempty` CHECK (char_length(`SourceKind`) > 0),
  CONSTRAINT `CK_DataImportBatchesStatus` CHECK (`ImportStatus` IN ('running', 'succeeded', 'failed')),
  CONSTRAINT `CK_DataImportBatchesTimeOrder` CHECK (`CompletedAt` IS NULL OR `CompletedAt` >= `StartedAt`),
  CONSTRAINT `CK_DataImportBatchesVersionNonempty` CHECK (char_length(`ImportVersion`) > 0),
  PRIMARY KEY (`ImportBatchId`),
  UNIQUE KEY `UX_DataImportBatchesSource` (`SourceKind`, `SourceDigest`, `ImportVersion`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='记录每个一致性快照或配置来源的导入身份、摘要、结果和对账报告。';

-- bf-step table DsNodes
CREATE TABLE `DsNodes` (
  `NodeId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '节点进程启动时分配的随机实例编号，重启不得复用。',
  `RegionId` INT NOT NULL COMMENT '受信任部署配置中的大区编号。',
  `NodeName` TEXT NOT NULL COMMENT '不含凭据的节点运维名称。',
  `LeaseExpiresAt` DATETIME(6) NOT NULL COMMENT '以数据库时钟计算的节点有效期限。',
  `Draining` BOOLEAN NOT NULL DEFAULT false COMMENT '节点是否停止接收新房间并等待已有房间结束。',
  CONSTRAINT `CK_DsNodesName` CHECK (length(`NodeName`) >= 1 AND length(`NodeName`) <= 128),
  CONSTRAINT `CK_DsNodesRegion` CHECK (`RegionId` > 0),
  PRIMARY KEY (`NodeId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='独立房间节点注册；节点租约过期后不再接受新分配。';

-- bf-step table EmailBindingSessions
CREATE TABLE `EmailBindingSessions` (
  `TokenDigest` VARBINARY(64) NOT NULL COMMENT '高熵邮箱操作凭证的SHA256摘要，不保存原凭证。',
  `AccountId` BIGINT NOT NULL COMMENT '已通过密码认证的凭证所属账号。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '凭证创建时间。',
  `ExpiresAt` DATETIME(6) NOT NULL COMMENT '凭证绝对到期时间。',
  `ConsumedAt` DATETIME(6) COMMENT '绑定成功后的消费时间，空值表示尚未消费。',
  CONSTRAINT `CK_EmailBindingExpiry` CHECK (`ExpiresAt` > `CreatedAt`),
  CONSTRAINT `CK_EmailBindingTokenDigest` CHECK (octet_length(`TokenDigest`) = 32),
  PRIMARY KEY (`TokenDigest`),
  KEY `IX_EmailBindingAccount` (`AccountId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='密码验证后签发的短期邮箱操作凭证，不允许兑换游戏票据。';

-- bf-step table EmailVerificationChallenges
CREATE TABLE `EmailVerificationChallenges` (
  `ChallengeId` VARCHAR(191) NOT NULL COMMENT '操作系统随机生成的128位挑战标识，不编码邮箱。',
  `Purpose` TEXT NOT NULL COMMENT '用途为注册或现有账号绑定，不能跨用途兑换。',
  `AccountId` BIGINT COMMENT '绑定挑战的现有账号主键；注册挑战为空。',
  `LoginKey` VARCHAR(64) NOT NULL COMMENT '验证码严格绑定的规范账号键。',
  `EmailAddress` TEXT NOT NULL COMMENT '发送邮件与成功绑定使用的邮箱地址。',
  `EmailKey` VARCHAR(254) NOT NULL COMMENT '邮箱比较键，用于白名单与发送频率限制。',
  `CodeDigest` LONGBLOB NOT NULL COMMENT '服务器私有密钥的HMAC摘要，不保存六位验证码正文。',
  `IpDigest` VARBINARY(64) NOT NULL COMMENT '来源IP的服务器密钥摘要，不记录原IP正文。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '挑战创建时间，同时作为发送限流统计依据。',
  `ExpiresAt` DATETIME(6) NOT NULL COMMENT '验证码绝对到期时间。',
  `AttemptCount` INT NOT NULL DEFAULT 0 COMMENT '已提交的错误验证码次数，错误结果也必须提交事务。',
  `AttemptLimit` INT NOT NULL COMMENT '此挑战签发时的最大猜测次数。',
  `Status` TEXT NOT NULL DEFAULT ('queued') COMMENT '排队、发送、失败、验证、消费或过期状态。',
  `VerifiedAt` DATETIME(6) COMMENT '正确验证码已被验证的时间。',
  `ConsumedAt` DATETIME(6) COMMENT '注册或绑定原子完成后的一次性消费时间。',
  `ProofDigest` VARBINARY(64) COMMENT '高熵注册授权的SHA256摘要，账号创建事务仅消费一次。',
  `ProofExpiresAt` DATETIME(6) COMMENT '注册授权的独立到期时间。',
  CONSTRAINT `CK_EmailChallengeAccountPurpose` CHECK (`Purpose` = 'register' AND `AccountId` IS NULL OR `Purpose` = 'bind' AND `AccountId` IS NOT NULL),
  CONSTRAINT `CK_EmailChallengeAddress` CHECK (octet_length(`EmailAddress`) >= 3 AND octet_length(`EmailAddress`) <= 254),
  CONSTRAINT `CK_EmailChallengeAttemptCount` CHECK (`AttemptCount` >= 0 AND `AttemptCount` <= 20),
  CONSTRAINT `CK_EmailChallengeAttemptLimit` CHECK (`AttemptLimit` >= 1 AND `AttemptLimit` <= 20),
  CONSTRAINT `CK_EmailChallengeCodeDigest` CHECK (octet_length(`CodeDigest`) = 32),
  CONSTRAINT `CK_EmailChallengeEmailKey` CHECK (octet_length(`EmailKey`) >= 3 AND octet_length(`EmailKey`) <= 254),
  CONSTRAINT `CK_EmailChallengeExpiry` CHECK (`ExpiresAt` > `CreatedAt`),
  CONSTRAINT `CK_EmailChallengeId` CHECK (REGEXP_LIKE(`ChallengeId`,'^[a-f0-9]{32}$','c')),
  CONSTRAINT `CK_EmailChallengeIpDigest` CHECK (octet_length(`IpDigest`) = 32),
  CONSTRAINT `CK_EmailChallengeLoginKey` CHECK (REGEXP_LIKE(`LoginKey`,'^[a-z0-9_]{3,24}$','c')),
  CONSTRAINT `CK_EmailChallengeProofDigest` CHECK (`ProofDigest` IS NULL OR octet_length(`ProofDigest`) = 32),
  CONSTRAINT `CK_EmailChallengeProofExpiry` CHECK ((`ProofDigest` IS NULL) = (`ProofExpiresAt` IS NULL)),
  CONSTRAINT `CK_EmailChallengePurpose` CHECK (`Purpose` IN ('register', 'bind')),
  CONSTRAINT `CK_EmailChallengeStatus` CHECK (`Status` IN ('queued', 'sent', 'failed', 'verified', 'consumed', 'expired')),
  PRIMARY KEY (`ChallengeId`),
  UNIQUE KEY `UX_EmailChallengeProofDigest` (`ProofDigest`),
  KEY `IX_EmailChallengeEmailTime` (`EmailKey`, `CreatedAt`),
  KEY `IX_EmailChallengeExpiry` (`ExpiresAt`),
  KEY `IX_EmailChallengeIpTime` (`IpDigest`, `CreatedAt`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='带发送状态、尝试次数与一次性注册授权的验证码挑战。';

-- bf-step table ForensicsAttachments
CREATE TABLE `ForensicsAttachments` (
  `AttachmentId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '附件标识。',
  `ReportId` BIGINT NOT NULL COMMENT '所属报告。',
  `Name` VARCHAR(128) NOT NULL COMMENT '客户端给出的附件名，用于区分类型。',
  `Compression` TEXT NOT NULL COMMENT '正文压缩方式。',
  `Payload` LONGBLOB NOT NULL COMMENT '压缩后的附件正文。',
  `PayloadBytes` BIGINT NOT NULL COMMENT '压缩后字节数。',
  `Sha256` VARCHAR(64) COMMENT '附件内容摘要；无法计算时为空。',
  `ReceivedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '服务端接收时间。',
  CONSTRAINT `CK_ForensicsAttachmentsBytes` CHECK (`PayloadBytes` > 0 AND `PayloadBytes` <= 67108864),
  CONSTRAINT `CK_ForensicsAttachmentsCompression` CHECK (`Compression` IN ('xpress-huffman', 'none')),
  CONSTRAINT `CK_ForensicsAttachmentsName` CHECK (char_length(`Name`) >= 1 AND char_length(`Name`) <= 128),
  CONSTRAINT `CK_ForensicsAttachmentsSha256` CHECK (`Sha256` IS NULL OR REGEXP_LIKE(`Sha256`,'^[0-9a-f]{64}$','c')),
  PRIMARY KEY (`AttachmentId`),
  KEY `IX_ForensicsAttachmentsReport` (`ReportId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='取证报告附带的二进制证据（例如截图）。';

-- bf-step table ForensicsReports
CREATE TABLE `ForensicsReports` (
  `ReportId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '报告标识。',
  `AccountId` BIGINT COMMENT '上报账号；未登录时为空。',
  `MachineCode` VARCHAR(69) COMMENT '上报设备摘要。',
  `ClientVersion` TEXT NOT NULL COMMENT '客户端守护构建标识，便于按版本排查。',
  `ReceivedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '服务端接收时间。',
  `GeneratedAt` DATETIME(6) COMMENT '客户端生成报告的时间；无法解析时为空。',
  `Compression` TEXT NOT NULL COMMENT '正文压缩方式。',
  `Payload` LONGBLOB NOT NULL COMMENT '压缩后的报告正文。',
  `PayloadBytes` BIGINT NOT NULL COMMENT '压缩后字节数。',
  `UncompressedBytes` BIGINT COMMENT '解压后字节数；未解压时为空。',
  `Summary` JSON NOT NULL DEFAULT ('{}') COMMENT '报告摘要字段，便于不解压即可检索。',
  `Verdict` VARCHAR(191) COMMENT '客户端自评判定；仅作参考，不作为处置依据。',
  `Score` INT COMMENT '客户端自评风险分；仅作参考。',
  CONSTRAINT `CK_ForensicsReportsBytes` CHECK (`PayloadBytes` > 0 AND `PayloadBytes` <= 67108864),
  CONSTRAINT `CK_ForensicsReportsClient` CHECK (char_length(`ClientVersion`) >= 1 AND char_length(`ClientVersion`) <= 64),
  CONSTRAINT `CK_ForensicsReportsCompression` CHECK (`Compression` IN ('xpress-huffman', 'none')),
  CONSTRAINT `CK_ForensicsReportsScore` CHECK (`Score` IS NULL OR `Score` >= 0 AND `Score` <= 100),
  CONSTRAINT `CK_ForensicsReportsTarget` CHECK (`AccountId` IS NOT NULL OR `MachineCode` IS NOT NULL),
  CONSTRAINT `CK_ForensicsReportsUncompressed` CHECK (`UncompressedBytes` IS NULL OR `UncompressedBytes` >= 0),
  CONSTRAINT `CK_ForensicsReportsVerdict` CHECK (`Verdict` IS NULL OR (`Verdict` IN ('clean', 'suspicious', 'cheating'))),
  PRIMARY KEY (`ReportId`),
  KEY `IX_ForensicsReportsAccount` (`AccountId`, `ReceivedAt` DESC),
  KEY `IX_ForensicsReportsMachine` (`MachineCode`, `ReceivedAt` DESC),
  KEY `IX_ForensicsReportsVerdict` (`Verdict`, `ReceivedAt` DESC)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='客户端取证报告存档：正文压缩入库，服务端不在本地生成报告文件。';

-- bf-step table FriendCategories
CREATE TABLE `FriendCategories` (
  `CategoryId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '服务端分配的分组编号',
  `OwnerPlayerId` BIGINT NOT NULL COMMENT '拥有此分组的玩家',
  `Name` VARCHAR(128) NOT NULL COMMENT '分组名称',
  CONSTRAINT `CK_FriendCategoriesName` CHECK (char_length(`Name`) >= 1 AND char_length(`Name`) <= 31),
  PRIMARY KEY (`CategoryId`),
  UNIQUE KEY `UX_FriendCategoriesName` (`OwnerPlayerId`, `Name`),
  UNIQUE KEY `UX_FriendCategoriesOwner` (`OwnerPlayerId`, `CategoryId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='玩家自有好友分组';

-- bf-step table FriendCategoryMembers
CREATE TABLE `FriendCategoryMembers` (
  `OwnerPlayerId` BIGINT NOT NULL COMMENT '拥有此分组的玩家',
  `FriendPlayerId` BIGINT NOT NULL COMMENT '已有好友的玩家编号',
  `CategoryId` BIGINT NOT NULL COMMENT '服务端分配的分组编号',
  CONSTRAINT `CK_FriendCategoryMembersDistinct` CHECK (`OwnerPlayerId` <> `FriendPlayerId`),
  PRIMARY KEY (`OwnerPlayerId`, `FriendPlayerId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='好友所属分组，一个好友仅归属一个分组';

-- bf-step table FriendRequests
CREATE TABLE `FriendRequests` (
  `SenderPlayerId` BIGINT NOT NULL COMMENT '发起好友申请的玩家。',
  `RecipientPlayerId` BIGINT NOT NULL COMMENT '接收并决定申请结果的玩家。',
  `RequestMessage` TEXT NOT NULL DEFAULT ('') COMMENT '可为空的申请附言；应用层按 UTF-16 代码单元和控制字符规则校验。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '申请首次建立的数据库时间，用于稳定排序。',
  `LegacyImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin COMMENT '旧 SQLite 待处理好友申请的来源导入批次；在线新申请为 NULL。',
  CONSTRAINT `CK_FriendRequestsDifferentPlayers` CHECK (`SenderPlayerId` <> `RecipientPlayerId`),
  CONSTRAINT `CK_FriendRequestsMessageLength` CHECK (char_length(`RequestMessage`) <= 256),
  PRIMARY KEY (`SenderPlayerId`, `RecipientPlayerId`),
  KEY `IX_FriendRequestsRecipientCreated` (`RecipientPlayerId`, `CreatedAt`, `SenderPlayerId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='当前待处理好友申请；方向由发送者指向接收者，接受或拒绝后删除当前行。';

-- bf-step table Friendships
CREATE TABLE `Friendships` (
  `FirstPlayerId` BIGINT NOT NULL COMMENT '有序好友对中较小的玩家编号。',
  `SecondPlayerId` BIGINT NOT NULL COMMENT '有序好友对中较大的玩家编号。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '好友申请被接受并建立关系的数据库时间。',
  `LegacyImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin COMMENT '旧 SQLite 好友关系的来源导入批次；在线新关系为 NULL。',
  `CreatedAtIsEstimated` BOOLEAN NOT NULL DEFAULT false COMMENT '旧表没有建立时间时为 true，created_at 仅记录迁移切点而非历史发生时刻。',
  CONSTRAINT `CK_FriendshipsLegacyTimeProvenance` CHECK (`CreatedAtIsEstimated` = (`LegacyImportBatchId` IS NOT NULL)),
  CONSTRAINT `CK_FriendshipsOrderedPair` CHECK (`FirstPlayerId` < `SecondPlayerId`),
  PRIMARY KEY (`FirstPlayerId`, `SecondPlayerId`),
  KEY `IX_FriendshipsSecondPlayer` (`SecondPlayerId`, `FirstPlayerId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='双向好友关系；始终以较小玩家编号在前保存，避免同一关系出现两个方向。';

-- bf-step table GroupChatInvitations
CREATE TABLE `GroupChatInvitations` (
  `ChatId` BIGINT NOT NULL COMMENT '群会话编号，使用高位空间避免与私聊编号冲突',
  `PlayerId` BIGINT NOT NULL COMMENT '成员或被邀请玩家',
  `InviterPlayerId` BIGINT NOT NULL COMMENT '发起邀请的成员',
  `ExpiresAt` DATETIME(6) NOT NULL DEFAULT (CURRENT_TIMESTAMP(6) + INTERVAL 3 MINUTE) COMMENT '邀请到期时间',
  CONSTRAINT `CK_GroupChatInvitationsDistinct` CHECK (`PlayerId` <> `InviterPlayerId`),
  PRIMARY KEY (`ChatId`, `PlayerId`),
  KEY `IX_GroupChatInvitationsPlayer` (`PlayerId`, `ExpiresAt`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='有期限且需接受的群邀请';

-- bf-step table GroupChatMembers
CREATE TABLE `GroupChatMembers` (
  `ChatId` BIGINT NOT NULL COMMENT '群会话编号，使用高位空间避免与私聊编号冲突',
  `PlayerId` BIGINT NOT NULL COMMENT '成员或被邀请玩家',
  `JoinedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '本次加入时间；历史消息可见性下限',
  PRIMARY KEY (`ChatId`, `PlayerId`),
  KEY `IX_GroupChatMembersPlayer` (`PlayerId`, `ChatId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='已同意加入的群成员';

-- bf-step table GroupChatMessages
CREATE TABLE `GroupChatMessages` (
  `MessageId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '单调增加的消息游标',
  `ChatId` BIGINT NOT NULL COMMENT '群会话编号，使用高位空间避免与私聊编号冲突',
  `SenderPlayerId` BIGINT NOT NULL COMMENT '认证后的发送者',
  `MessageText` TEXT NOT NULL COMMENT '经敏感词和 Unicode 校验的正文',
  `SentAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '消息提交时间',
  CONSTRAINT `CK_GroupChatMessagesMessageText` CHECK (char_length(`MessageText`) >= 1 AND char_length(`MessageText`) <= 256),
  PRIMARY KEY (`MessageId`),
  KEY `IX_GroupChatMessagesHistory` (`ChatId`, `MessageId` DESC)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='已提交的群消息与历史游标';

-- bf-step table GroupChats
CREATE TABLE `GroupChats` (
  `ChatId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '群会话编号，使用高位空间避免与私聊编号冲突',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '群聊创建时间',
  PRIMARY KEY (`ChatId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin AUTO_INCREMENT=2147483648 COMMENT='原版 NM 窗口的多人通讯会话';

-- bf-step table HordeTutorSessions
CREATE TABLE `HordeTutorSessions` (
  `TutorialSessionId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '执行器分配的独立UUID，非客户端指定或正式比赛ID。',
  `PlayerId` BIGINT NOT NULL COMMENT '网关认证玩家；账本保留历史，禁止级联删除。',
  `ModeId` INT NOT NULL DEFAULT 103 COMMENT '固定原HordeTutor模式103。',
  `MapKey` TEXT NOT NULL DEFAULT ('rel_prologue_01') COMMENT '唯一启用原生资源rel_prologue_01。',
  `Difficulty` INT NOT NULL DEFAULT 1 COMMENT '原资源固定Difficulty1，不是任务stageNum或进度键。',
  `Authority` TEXT NOT NULL DEFAULT ('client_reported') COMMENT '永远client_reported；声明不代表服务端观察NPC死亡。',
  `ProfileHash` TEXT NOT NULL COMMENT '开始时冻结的原生证据profile SHA256，不信任客户端摘要。',
  `SessionStatus` TEXT NOT NULL DEFAULT ('started') COMMENT 'started、reported_clear或interrupted，终态不能覆写。',
  `StartedAt` DATETIME(6) NOT NULL DEFAULT (CURRENT_TIMESTAMP(6)) COMMENT 'SQL事务的实际开始时刻，不用客户端时钟。',
  `FinishedAt` DATETIME(6) COMMENT '首次终态提交时刻，幂等重放保持不变。',
  `TerminalReason` TEXT COMMENT '客户端声明通关或明确的传输/离场/断连中断原因。',
  `ExperienceAwarded` BIGINT NOT NULL DEFAULT 0 COMMENT '未知原服奖励，本服固定零经验。',
  `LucciAwarded` BIGINT NOT NULL DEFAULT 0 COMMENT '本服固定零金币，不连接钱包事务。',
  `RatingDelta` BIGINT NOT NULL DEFAULT 0 COMMENT '本服固定零段位变化，不是竞技胜负。',
  `ItemAwards` JSON NOT NULL DEFAULT (JSON_ARRAY()) COMMENT '本服固定空奖励，不从MapMeta IsItemAward字段猜出物品。',
  CONSTRAINT `CK_HordeTutorAuthority` CHECK (`Authority` = 'client_reported'),
  CONSTRAINT `CK_HordeTutorDifficulty` CHECK (`Difficulty` = 1),
  CONSTRAINT `CK_HordeTutorMap` CHECK (`MapKey` = 'rel_prologue_01'),
  CONSTRAINT `CK_HordeTutorMode` CHECK (`ModeId` = 103),
  CONSTRAINT `CK_HordeTutorProfile` CHECK (REGEXP_LIKE(`ProfileHash`,'^[0-9a-f]{64}$','c')),
  CONSTRAINT `CK_HordeTutorReason` CHECK (REGEXP_LIKE(`TerminalReason`,'^[a-z][a-z0-9_]{0,63}$','c')),
  CONSTRAINT `CK_HordeTutorReportReason` CHECK (`SessionStatus` <> 'reported_clear' OR `TerminalReason` = 'client_reported_clear'),
  CONSTRAINT `CK_HordeTutorStatus` CHECK (`SessionStatus` IN ('started', 'reported_clear', 'interrupted')),
  CONSTRAINT `CK_HordeTutorTerminal` CHECK (`SessionStatus` = 'started' AND `FinishedAt` IS NULL AND `TerminalReason` IS NULL OR `SessionStatus` <> 'started' AND `FinishedAt` IS NOT NULL AND `TerminalReason` IS NOT NULL AND `FinishedAt` >= `StartedAt`),
  CONSTRAINT `CK_HordeTutorZeroItems` CHECK (`ItemAwards` = JSON_ARRAY()),
  CONSTRAINT `CK_HordeTutorZeroLucci` CHECK (`LucciAwarded` = 0),
  CONSTRAINT `CK_HordeTutorZeroRating` CHECK (`RatingDelta` = 0),
  CONSTRAINT `CK_HordeTutorZeroXp` CHECK (`ExperienceAwarded` = 0),
  PRIMARY KEY (`TutorialSessionId`),
  KEY `IX_HordeTutorPlayerHistory` (`PlayerId`, `StartedAt` DESC),
  `ConditionalUqHordeTutorLivePlayerPlayerId` BIGINT GENERATED ALWAYS AS (CASE WHEN (`SessionStatus` = 'started') THEN `PlayerId` ELSE NULL END) STORED,
  UNIQUE KEY `UX_HordeTutorLivePlayer` (`ConditionalUqHordeTutorLivePlayerPlayerId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='原103本地教程的客户端声明；非服务端独立验证NPC击杀，不产生竞技统计或奖励。';

-- bf-step table InstantRespawnUses
CREATE TABLE `InstantRespawnUses` (
  `MatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '世界线程冻结且已经持久化的对局 UUID。',
  `PlayerId` BIGINT NOT NULL COMMENT '经过连接认证的扣费玩家。',
  `LifeGeneration` BIGINT NOT NULL COMMENT '本局当前生命代次，与玩家及对局构成幂等键。',
  `BusinessId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '与库存流水关联的唯一业务 UUID。',
  `ConsumedAt` DATETIME(6) NOT NULL COMMENT '库存扣减提交采用的服务端 UTC 时间。',
  CONSTRAINT `CK_InstantRespawnUsesLifePositive` CHECK (`LifeGeneration` > 0),
  PRIMARY KEY (`MatchId`, `PlayerId`, `LifeGeneration`),
  UNIQUE KEY `UX_InstantRespawnUsesBusiness` (`BusinessId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='714 即时复活扣费凭据：和库存、库存流水同事务提交，重复请求不再次扣除。';

-- bf-step table InventoryDiscards
CREATE TABLE `InventoryDiscards` (
  `DiscardId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '服务端分配的事务UUID，兼作唯一库存流水业务号。',
  `PlayerId` BIGINT NOT NULL COMMENT '认证物品主人；不接受载荷玩家身份。',
  `NativeItemKey` BIGINT NOT NULL COMMENT '802原库存行键；当前正常lane为本人唯一模板映射，不是任意数据库实例号。',
  `ItemInstanceId` BIGINT NOT NULL COMMENT '删除之前冻结的精确库存实例主键，删除后仍保留历史身份。',
  `RemovedQuantity` BIGINT NOT NULL COMMENT '过期实例原有正数量；不是客户端自行指定删弃数量。',
  `AcquiredAt` DATETIME(6) NOT NULL COMMENT '冻结的原资产获得时刻。',
  `ExpiresAt` DATETIME(6) NOT NULL COMMENT '冻结的到期时刻，必须已经到期。',
  `DiscardedAt` DATETIME(6) NOT NULL COMMENT '唯一删除事务统一UTC时刻。',
  `NativeFlags` INT NOT NULL DEFAULT 2 COMMENT '原7B04F0的expired位2；不伪造租借、网吧或其他未知控制位。',
  CONSTRAINT `CK_InventoryDiscardsExpired` CHECK (`AcquiredAt` < `ExpiresAt` AND `ExpiresAt` <= `DiscardedAt`),
  CONSTRAINT `CK_InventoryDiscardsFlags` CHECK (`NativeFlags` = 2),
  CONSTRAINT `CK_InventoryDiscardsInstance` CHECK (`ItemInstanceId` > 0),
  CONSTRAINT `CK_InventoryDiscardsNativeKey` CHECK (`NativeItemKey` >= 1 AND `NativeItemKey` <= 2147483647 AND (`NativeItemKey` < 301000 OR `NativeItemKey` > 399999)),
  CONSTRAINT `CK_InventoryDiscardsQuantity` CHECK (`RemovedQuantity` >= 1 AND `RemovedQuantity` <= 2147483647),
  PRIMARY KEY (`DiscardId`),
  UNIQUE KEY `UX_InventoryDiscardsInstance` (`PlayerId`, `ItemInstanceId`),
  KEY `IX_InventoryDiscardsNative` (`PlayerId`, `NativeItemKey`, `DiscardedAt`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='原808的本人已过期实例删除回执；精确实例与数量冻结，无可用资产或钱包授权。';

-- bf-step table InventoryTransactions
CREATE TABLE `InventoryTransactions` (
  `InventoryTransactionId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '库存流水递增主键。',
  `PlayerId` BIGINT NOT NULL COMMENT '库存变更所属玩家。',
  `ItemId` BIGINT NOT NULL COMMENT '变更涉及的物品模板编号。',
  `ItemInstanceId` BIGINT COMMENT '可关联的当前库存实例；纯历史或已移除实例允许为 NULL。',
  `QuantityDelta` BIGINT NOT NULL DEFAULT 0 COMMENT '本次有符号数量变化；仅续期时可以为零。',
  `ExpiresBefore` DATETIME(6) COMMENT '变更前到期时间；永久或首次发放可为 NULL。',
  `ExpiresAfter` DATETIME(6) COMMENT '变更后到期时间；永久或彻底移除可为 NULL。',
  `ReasonCode` VARCHAR(191) NOT NULL COMMENT '购买、初始发放、消耗、导入期初或补偿等稳定原因代码。',
  `BusinessId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '产生该变更的稳定业务 UUID。',
  `OccurredAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '库存变更提交时间。',
  `ImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin COMMENT '迁移期初流水的来源批次；在线业务为 NULL。',
  CONSTRAINT `CK_InventoryTransactionsChange` CHECK (`QuantityDelta` <> 0 OR NOT (`ExpiresBefore` <=> `ExpiresAfter`)),
  CONSTRAINT `CK_InventoryTransactionsReasonNonempty` CHECK (char_length(`ReasonCode`) > 0),
  PRIMARY KEY (`InventoryTransactionId`),
  UNIQUE KEY `UX_InventoryTransactionsBusiness` (`BusinessId`, `ItemId`, `PlayerId`, `ReasonCode`),
  KEY `IX_InventoryTransactionsPlayerTime` (`PlayerId`, `OccurredAt`, `InventoryTransactionId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='库存不可变增减、续期、合并与补偿流水；不代表当前库存快照。';

-- bf-step table ItemTemplates
CREATE TABLE `ItemTemplates` (
  `ItemId` BIGINT NOT NULL COMMENT '客户端物品模板编号，必须为正。',
  `ItemName` TEXT NOT NULL COMMENT '当前配置版本中的物品显示名称。',
  `SlotCode` VARCHAR(64) COMMENT '装备槽代码；不可装备物品为 NULL。',
  `StackPolicy` TEXT NOT NULL COMMENT '库存合并策略：merge_quantity 或 single_instance。',
  `DurationPolicy` TEXT NOT NULL COMMENT '期限策略：permanent、expiring 或 offer_defined。',
  `MaxQuantity` BIGINT NOT NULL COMMENT '同模板当前库存允许的最大数量；单实例物品固定为 1。',
  `MaxDurationSeconds` BIGINT COMMENT '从当前时刻计算允许累积的最大期限秒数；永久或数量堆叠物品为 NULL。',
  `ClientFields` JSON NOT NULL DEFAULT ('{}') COMMENT '已验证但尚不适合拆列的客户端模板字段，并带随配置发布的结构语义。',
  `ConfigurationVersionId` BIGINT NOT NULL COMMENT '产生该模板的配置版本。',
  `IsGrantable` BOOLEAN NOT NULL DEFAULT true COMMENT '是否允许库存发放；客户端合并上限为零的已知模板保留元数据但不得购买或奖励。',
  CONSTRAINT `CK_ItemTemplatesClientFieldsObject` CHECK (LOWER(JSON_TYPE(`ClientFields`)) = 'object'),
  CONSTRAINT `CK_ItemTemplatesDurationLimits` CHECK (`DurationPolicy` = 'permanent' AND `MaxDurationSeconds` IS NULL OR (`DurationPolicy` IN ('expiring', 'offer_defined')) AND `MaxDurationSeconds` IS NOT NULL),
  CONSTRAINT `CK_ItemTemplatesDurationPolicy` CHECK (`DurationPolicy` IN ('permanent', 'expiring', 'offer_defined')),
  CONSTRAINT `CK_ItemTemplatesGrantability` CHECK (`IsGrantable` AND `MaxQuantity` > 0 OR NOT `IsGrantable` AND `MaxQuantity` = 0),
  CONSTRAINT `CK_ItemTemplatesIdPositive` CHECK (`ItemId` > 0),
  CONSTRAINT `CK_ItemTemplatesMaxDurationPositive` CHECK (`MaxDurationSeconds` IS NULL OR `MaxDurationSeconds` > 0),
  CONSTRAINT `CK_ItemTemplatesMaxQuantityNonnegative` CHECK (`MaxQuantity` >= 0),
  CONSTRAINT `CK_ItemTemplatesNameNonempty` CHECK (char_length(`ItemName`) > 0),
  CONSTRAINT `CK_ItemTemplatesSlotLength` CHECK (`SlotCode` IS NULL OR char_length(`SlotCode`) >= 1 AND char_length(`SlotCode`) <= 64),
  CONSTRAINT `CK_ItemTemplatesStackLimits` CHECK (`StackPolicy` = 'merge_quantity' AND `MaxDurationSeconds` IS NULL OR `StackPolicy` = 'single_instance' AND (`MaxQuantity` IN (0, 1))),
  CONSTRAINT `CK_ItemTemplatesStackPolicy` CHECK (`StackPolicy` IN ('merge_quantity', 'single_instance')),
  PRIMARY KEY (`ItemId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='版本化物品模板；与可购买的 stock 销售规格分离。';

-- bf-step table LifeAccountingEpoch
CREATE TABLE `LifeAccountingEpoch` (
  `Singleton` BOOLEAN NOT NULL DEFAULT true COMMENT '唯一部署结算起点记录。',
  `StartedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '本迁移生效时间；此前日期缺少可靠历史，不追扣。',
  `JournalEpoch` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL DEFAULT (UUID()) COMMENT '本库固定出席日志纪元，用于跨进程恢复及隔离其他数据库，不含连接凭据。',
  CONSTRAINT `CK_LifeAccountingSingleton` CHECK (`Singleton` = TRUE),
  PRIMARY KEY (`Singleton`),
  UNIQUE KEY `UX_LifeJournalEpoch` (`JournalEpoch`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='Life出席结算起点；升级前无可靠出席历史的日期不追扣。';

-- bf-step table LifeAttendanceDays
CREATE TABLE `LifeAttendanceDays` (
  `PlayerId` BIGINT NOT NULL COMMENT '认证游戏会话的玩家身份。',
  `AttendanceDate` DATE NOT NULL COMMENT 'UTC+8自然日，来源为实际认证连接观察。',
  `RecordedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '该日期首次持久化时间，重报保持不变。',
  PRIMARY KEY (`PlayerId`, `AttendanceDate`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='认证游戏会话实际出席日期，UTC+8自然日；独立于每日钱包奖励与他人资料读取。';

-- bf-step table LifeGuardianEvents
CREATE TABLE `LifeGuardianEvents` (
  `EventId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '守护者事件顺序，不由客户端提供。',
  `PlayerId` BIGINT NOT NULL COMMENT '库存所属玩家。',
  `ItemInstanceId` BIGINT NOT NULL COMMENT '历史守护者实例编号，删除库存仍保留事件。',
  `EffectiveAt` DATETIME(6) NOT NULL COMMENT '库存事务实际发生时间。',
  `ExpiresAt` DATETIME(6) NOT NULL COMMENT '该事件生效后的到期时间；删除等于生效时间。',
  `EventKind` TEXT NOT NULL COMMENT '迁移基线、获得、续期变更或移除。',
  CONSTRAINT `CK_LifeGuardianKind` CHECK (`EventKind` IN ('baseline', 'grant', 'change', 'remove')),
  PRIMARY KEY (`EventId`),
  KEY `IX_LifeGuardianPlayer` (`PlayerId`, `ItemInstanceId`, `EventId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='生命守护者102141实际库存变更历史；续期不覆盖过期空档，删除保留历史，不从首次获得时间倒推。';

-- bf-step table LifeItemCursors
CREATE TABLE `LifeItemCursors` (
  `ItemInstanceId` BIGINT NOT NULL COMMENT '原始Life库存实例，重新获得不能继承旧实例游标。',
  `NextDate` DATE NOT NULL COMMENT '第一个尚未结算的自然日。',
  PRIMARY KEY (`ItemInstanceId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='每个Life实例首个未结算日期；零Life也推进，补充后不重复追扣旧缺席。';

-- bf-step table LifeItemSettlements
CREATE TABLE `LifeItemSettlements` (
  `ItemInstanceId` BIGINT NOT NULL COMMENT '结算时的精确Life实例。',
  `PlayerId` BIGINT NOT NULL COMMENT '资产所属玩家。',
  `ItemId` BIGINT NOT NULL COMMENT '原始Life模板编号。',
  `FirstDate` DATE NOT NULL COMMENT '本次结算首日，包含该日。',
  `ThroughDate` DATE NOT NULL COMMENT '本次结算末日，最多为昨天。',
  `AbsentDays` INT NOT NULL COMMENT '没有出席记录且无守护者覆盖的自然日数。',
  `LifeBefore` INT NOT NULL COMMENT '事务锁定后的扣减前Life。',
  `LifeAfter` INT NOT NULL COMMENT '扣减后Life，最低为零，库存不删除。',
  `BusinessId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '关联实际库存扣减流水的独立业务号。',
  `SettledAt` DATETIME(6) NOT NULL COMMENT '服务器结算观测时间。',
  CONSTRAINT `CK_LifeSettlementAbsences` CHECK (`AbsentDays` >= 0 AND `AbsentDays` <= (`ThroughDate` - `FirstDate` + 1)),
  CONSTRAINT `CK_LifeSettlementAfter` CHECK (`LifeAfter` = GREATEST(0, `LifeBefore` - `AbsentDays`)),
  CONSTRAINT `CK_LifeSettlementBefore` CHECK (`LifeBefore` >= 0 AND `LifeBefore` <= 15),
  CONSTRAINT `CK_LifeSettlementDates` CHECK (`ThroughDate` >= `FirstDate`),
  CONSTRAINT `CK_LifeSettlementItem` CHECK (`ItemId` IN (
  101447,101448,101449,101450,101451,101452,101453,101454,101455,101700,
  101701,101702,101703,101704,101786,101856,101862,101920,101922,101956,
  101987,101999,102029,102068,102069,102070,102073,102089,102115,102147,
  102265,102314,102315,102316,102317,102318,102369,190149,190150,190155,
  190157,190175,190202,190244,201337,201350,201359,201362,201923,201924,
  202339,202386,202394,202400,202924,202938,202939,202956,203381,203390,
  203395,203925,203926,203934,203935,203949,204131,204375,204382,204388,
  204924,204933,204934,204944,207925,207928,207941,207946,207952,207975
 )),
  PRIMARY KEY (`ItemInstanceId`, `ThroughDate`),
  UNIQUE KEY `UX_LifeSettlementBusiness` (`BusinessId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='不可变Life缺席结算收据；自然日区间、未保护缺席天数和真实库存扣减，零扣除同样推进。';

-- bf-step table LoginTickets
CREATE TABLE `LoginTickets` (
  `TicketDigest` VARBINARY(64) NOT NULL COMMENT '票据正文的 32 字节 SHA-256 摘要，同时作为主键。',
  `AccountId` BIGINT NOT NULL COMMENT '票据绑定的账号。',
  `ExpiresAt` DATETIME(6) NOT NULL COMMENT '票据绝对失效时间；消费时必须在同一事务内复核。',
  `ConsumedAt` DATETIME(6) COMMENT '成功消费时间；NULL 表示尚未消费。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '票据签发的数据库时间。',
  `RegionId` INT NOT NULL DEFAULT 1 COMMENT '票据允许进入的大区编号；旧数据与单机部署默认为一区。',
  `MachineCode` VARCHAR(69) COMMENT '登录票据绑定的设备摘要；旧票据须重新登录。',
  CONSTRAINT `CK_LoginTicketsConsumedOrder` CHECK (`ConsumedAt` IS NULL OR `ConsumedAt` >= `CreatedAt`),
  CONSTRAINT `CK_LoginTicketsDigestSha256` CHECK (octet_length(`TicketDigest`) = 32),
  CONSTRAINT `CK_LoginTicketsExpiryOrder` CHECK (`ExpiresAt` > `CreatedAt`),
  CONSTRAINT `CK_LoginTicketsRegionId` CHECK (`RegionId` > 0),
  PRIMARY KEY (`TicketDigest`),
  KEY `IX_LoginTicketsAccountExpiry` (`AccountId`, `ExpiresAt`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='短期、单次消费的游戏登录票据摘要；原票据正文永不入库。';

-- bf-step table MachineDevices
CREATE TABLE `MachineDevices` (
  `MachineCode` VARCHAR(69) NOT NULL COMMENT '带算法版本的设备摘要。',
  `FirstSeenAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '首次成功登录时间。',
  `LastSeenAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '最近成功登录时间。',
  CONSTRAINT `CK_MachineDevicesCode` CHECK (REGEXP_LIKE(`MachineCode`,'^win1:[0-9a-f]{64}$','c')),
  PRIMARY KEY (`MachineCode`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='游戏专用设备摘要；不保存原始机器标识。';

-- bf-step table MatchCoupleBenefits
CREATE TABLE `MatchCoupleBenefits` (
  `MatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '耐久对局身份。',
  `PlayerId` BIGINT NOT NULL COMMENT '本方玩家。',
  `PartnerPlayerId` BIGINT NOT NULL COMMENT '同场伴侣。',
  `CoupleId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '开局情侣身份，独立于后续关系删除。',
  `RingItemId` BIGINT NOT NULL COMMENT '原版明确具有50%加成的正式戒指。',
  CONSTRAINT `CK_MatchCoupleBenefitsDistinct` CHECK (`PlayerId` <> `PartnerPlayerId`),
  CONSTRAINT `CK_MatchCoupleBenefitsRing` CHECK (`RingItemId` IN (100985, 101396)),
  PRIMARY KEY (`MatchId`, `PlayerId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='开局冻结的已核实高级戒指权益；解除情侣后保留历史。';

-- bf-step table MatchParticipants
CREATE TABLE `MatchParticipants` (
  `MatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '所属对局 UUID。',
  `PlayerId` BIGINT NOT NULL COMMENT '参与玩家；同一对局每名玩家至多一行。',
  `SeatIndex` INT NOT NULL COMMENT '开局冻结的零起始席位，用于对象编号和客户端布局。',
  `TeamCode` INT NOT NULL COMMENT '开局冻结的队伍代码；不同模式可使用已发布范围内的值。',
  `ParticipantStatus` TEXT NOT NULL DEFAULT ('joined') COMMENT '参与状态：joined、disconnected 或 completed。',
  `Statistics` JSON NOT NULL DEFAULT ('{}') COMMENT '有来源版本的终局统计 JSON 对象，未知字段不凭空补齐。',
  `JoinedAt` DATETIME(6) NOT NULL COMMENT '玩家进入本局快照的时间。',
  `LeftAt` DATETIME(6) COMMENT '离开、断线或完成时间；仍在局内时为 NULL。',
  CONSTRAINT `CK_MatchParticipantsSeatRange` CHECK (`SeatIndex` >= 0 AND `SeatIndex` <= 255),
  CONSTRAINT `CK_MatchParticipantsStatisticsObject` CHECK (LOWER(JSON_TYPE(`Statistics`)) = 'object'),
  CONSTRAINT `CK_MatchParticipantsStatus` CHECK (`ParticipantStatus` IN ('joined', 'disconnected', 'completed')),
  CONSTRAINT `CK_MatchParticipantsTeamRange` CHECK (`TeamCode` >= 0 AND `TeamCode` <= 255),
  CONSTRAINT `CK_MatchParticipantsTimeOrder` CHECK (`LeftAt` IS NULL OR `LeftAt` >= `JoinedAt`),
  PRIMARY KEY (`MatchId`, `PlayerId`),
  UNIQUE KEY `UX_MatchParticipantsSeat` (`MatchId`, `SeatIndex`),
  KEY `IX_MatchParticipantsPlayerMatch` (`PlayerId`, `MatchId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='对局参与者席位、队伍和终局统计快照；不从后来修改的玩家资料重建。';

-- bf-step table MatchRewardGrants
CREATE TABLE `MatchRewardGrants` (
  `RewardGrantId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '奖励项稳定 UUID。',
  `SettlementId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '所属结算操作。',
  `MatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '所属对局，并与玩家组成参与者复合外键。',
  `PlayerId` BIGINT NOT NULL COMMENT '获得奖励的对局参与玩家。',
  `RewardKey` VARCHAR(128) NOT NULL COMMENT '规则版本内稳定奖励项代码，用于同局同玩家去重。',
  `CurrencyCode` VARCHAR(191) COMMENT '货币奖励的资产代码；物品奖励时为 NULL。',
  `Amount` BIGINT COMMENT '正整数货币奖励数量；物品奖励时为 NULL。',
  `ItemId` BIGINT COMMENT '物品奖励模板；货币奖励时为 NULL。',
  `Quantity` BIGINT COMMENT '正整数物品数量；货币奖励时为 NULL。',
  `GrantStatus` VARCHAR(191) NOT NULL DEFAULT 'pending' COMMENT '发放状态：pending、granted 或 failed。',
  `WalletTransactionId` BIGINT COMMENT '货币奖励提交后的钱包流水；其他情况为 NULL。',
  `InventoryTransactionId` BIGINT COMMENT '物品奖励提交后的库存流水；其他情况为 NULL。',
  `GrantedAt` DATETIME(6) COMMENT '奖励成功发放时间；未成功时为 NULL。',
  CONSTRAINT `CK_MatchRewardGrantsCompletion` CHECK (`GrantStatus` = 'granted' AND `GrantedAt` IS NOT NULL OR `GrantStatus` <> 'granted' AND `GrantedAt` IS NULL),
  CONSTRAINT `CK_MatchRewardGrantsCurrency` CHECK (`CurrencyCode` IS NULL OR (`CurrencyCode` IN ('lucci', 'bubble_cash', 'treasure_bubble'))),
  CONSTRAINT `CK_MatchRewardGrantsKeyNonempty` CHECK (char_length(`RewardKey`) > 0),
  CONSTRAINT `CK_MatchRewardGrantsStatus` CHECK (`GrantStatus` IN ('pending', 'granted', 'failed')),
  CONSTRAINT `CK_MatchRewardGrantsValue` CHECK (`CurrencyCode` IS NOT NULL AND `Amount` IS NOT NULL AND `Amount` > 0 AND `ItemId` IS NULL AND `Quantity` IS NULL OR `CurrencyCode` IS NULL AND `Amount` IS NULL AND `ItemId` IS NOT NULL AND `Quantity` IS NOT NULL AND `Quantity` > 0),
  PRIMARY KEY (`RewardGrantId`),
  UNIQUE KEY `UX_MatchRewardGrantsBusiness` (`MatchId`, `PlayerId`, `RewardKey`),
  KEY `IX_MatchRewardGrantsSettlementStatus` (`SettlementId`, `GrantStatus`, `RewardGrantId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='对局结算中每名玩家每个奖励项的幂等结果；货币和物品奖励严格二选一。';

-- bf-step table MatchSettlements
CREATE TABLE `MatchSettlements` (
  `SettlementId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '结算操作稳定 UUID。',
  `MatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '被结算的唯一对局。',
  `SettlementStatus` VARCHAR(191) NOT NULL DEFAULT 'pending' COMMENT '状态：pending、processing、succeeded、failed_retryable 或 failed_terminal。',
  `TerminalSnapshot` JSON NOT NULL COMMENT '结算输入的不可变终局快照；必须是 JSON 对象。',
  `RewardRulesVersionId` BIGINT NOT NULL COMMENT '计算奖励采用的已发布规则版本。',
  `AttemptCount` INT NOT NULL DEFAULT 0 COMMENT '开始处理的累计次数，用于有界重试和告警。',
  `FailureCode` TEXT COMMENT '稳定失败分类；仅失败状态非空，不保存异常秘密。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '结算记录首次建立时间。',
  `SettledAt` DATETIME(6) COMMENT '成功结算提交时间；未成功时为 NULL。',
  `ResultSnapshot` JSON COMMENT '成功提交时冻结的确定性结算结果，供同一终局幂等重放校验；其他状态必须为 NULL。',
  `ProcessingStartedAt` DATETIME(6) COMMENT '最近一次开始处理的时间；只有 processing 状态非空，恢复任务据此识别旧实现遗留的孤儿。',
  `UpdatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '结算状态、尝试次数或结果最近一次事务提交时间。',
  CONSTRAINT `CK_MatchSettlementsAttemptNonnegative` CHECK (`AttemptCount` >= 0),
  CONSTRAINT `CK_MatchSettlementsCompletion` CHECK (`SettlementStatus` = 'succeeded' AND `SettledAt` IS NOT NULL AND `FailureCode` IS NULL OR `SettlementStatus` <> 'succeeded' AND `SettledAt` IS NULL),
  CONSTRAINT `CK_MatchSettlementsFailureCode` CHECK ((`SettlementStatus` IN ('failed_retryable', 'failed_terminal')) AND `FailureCode` IS NOT NULL AND char_length(`FailureCode`) > 0 OR (`SettlementStatus` NOT IN ('failed_retryable', 'failed_terminal')) AND `FailureCode` IS NULL),
  CONSTRAINT `CK_MatchSettlementsProcessingTime` CHECK (`SettlementStatus` = 'processing' AND `ProcessingStartedAt` IS NOT NULL OR `SettlementStatus` <> 'processing' AND `ProcessingStartedAt` IS NULL),
  CONSTRAINT `CK_MatchSettlementsResultSnapshot` CHECK (`ResultSnapshot` IS NULL OR LOWER(JSON_TYPE(`ResultSnapshot`)) = 'object'),
  CONSTRAINT `CK_MatchSettlementsResultState` CHECK (`SettlementStatus` = 'succeeded' AND `ResultSnapshot` IS NOT NULL OR `SettlementStatus` <> 'succeeded' AND `ResultSnapshot` IS NULL),
  CONSTRAINT `CK_MatchSettlementsSnapshotObject` CHECK (LOWER(JSON_TYPE(`TerminalSnapshot`)) = 'object'),
  CONSTRAINT `CK_MatchSettlementsStatus` CHECK (`SettlementStatus` IN ('pending', 'processing', 'succeeded', 'failed_retryable', 'failed_terminal')),
  PRIMARY KEY (`SettlementId`),
  UNIQUE KEY `UX_MatchSettlementsMatch` (`MatchId`),
  KEY `IX_MatchSettlementsRecovery` (`SettlementStatus`, `CreatedAt`, `SettlementId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='每局唯一的可靠结算记录；固定终局和规则后可按 match_id 查询、重试且不重复发奖。';

-- bf-step table Matches
CREATE TABLE `Matches` (
  `MatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '服务端在开局时生成的稳定对局 UUID。',
  `SourceRoomId` BIGINT NOT NULL COMMENT '创建对局的进程内房间编号，仅用于追踪且不得作为全局主键。',
  `ModeId` INT NOT NULL COMMENT '对局模式编号；协议数字在边界映射，未知模式不得假装已支持。',
  `MatchStatus` VARCHAR(191) NOT NULL COMMENT '生命周期：loading、playing、completed、interrupted、settlement_pending 或 settled。',
  `ConfigurationVersionId` BIGINT NOT NULL COMMENT '对局采用的已发布配置版本。',
  `RulesSnapshot` JSON NOT NULL COMMENT '开局时冻结的地图、模式和奖励规则引用；必须是 JSON 对象。',
  `StartedAt` DATETIME(6) NOT NULL COMMENT '对局加载或正式开始的 UTC 时间。',
  `EndedAt` DATETIME(6) COMMENT '完成或中断时间；仍在加载/战斗时为 NULL。',
  `LegacyMatchId` VARCHAR(191) COMMENT '旧 SQLite match_results.match_id；新对局为 NULL。',
  `LegacyImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin COMMENT '旧历史对局快照的来源导入批次；导入历史不会重新结算发奖。',
  `RoomInstanceId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin COMMENT '独立房间的实例归属；空值只用于兼容本地执行后端。',
  `RoomOwnerEpoch` BIGINT COMMENT '开局冻结的所有权代次，历史事实不随续租改写。',
  CONSTRAINT `CK_MatchesModePositive` CHECK (`ModeId` > 0),
  CONSTRAINT `CK_MatchesRoomOwner` CHECK (`RoomInstanceId` IS NULL AND `RoomOwnerEpoch` IS NULL OR `RoomInstanceId` IS NOT NULL AND `RoomOwnerEpoch` IS NOT NULL AND `RoomOwnerEpoch` > 0),
  CONSTRAINT `CK_MatchesRoomPositive` CHECK (`SourceRoomId` > 0),
  CONSTRAINT `CK_MatchesRulesObject` CHECK (LOWER(JSON_TYPE(`RulesSnapshot`)) = 'object'),
  CONSTRAINT `CK_MatchesStatus` CHECK (`MatchStatus` IN ('loading', 'playing', 'completed', 'interrupted', 'settlement_pending', 'settled')),
  CONSTRAINT `CK_MatchesTerminalTime` CHECK ((`MatchStatus` IN ('loading', 'playing')) AND `EndedAt` IS NULL OR (`MatchStatus` NOT IN ('loading', 'playing')) AND `EndedAt` IS NOT NULL),
  CONSTRAINT `CK_MatchesTimeOrder` CHECK (`EndedAt` IS NULL OR `EndedAt` >= `StartedAt`),
  PRIMARY KEY (`MatchId`),
  UNIQUE KEY `UX_MatchesLegacy` (`LegacyMatchId`),
  KEY `IX_MatchesRoomInstance` (`RoomInstanceId`, `RoomOwnerEpoch`),
  KEY `IX_MatchesStatusEnded` (`MatchStatus`, `EndedAt`, `MatchId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='对局身份、生命周期和不可变规则快照；进程崩溃后只按已持久化终局继续结算。';

-- bf-step table MegaphoneMessages
CREATE TABLE `MegaphoneMessages` (
  `BusinessId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '服务端生成的稳定操作标识，与库存流水关联。',
  `PlayerId` BIGINT NOT NULL COMMENT '扣除本人库存的已认证玩家。',
  `ChannelId` INT NOT NULL COMMENT '接收请求时世界线程冻结的频道。',
  `RequestType` INT NOT NULL COMMENT '客户端请求类型，用于严格比较重试参数。',
  `SelectionId` INT NOT NULL COMMENT '原始自动选择值或显式道具模板，参与短时重复判断。',
  `ItemId` BIGINT NOT NULL COMMENT '事务实际扣除的喇叭模板，控制广播范围和原生颜色。',
  `SenderName` TEXT NOT NULL COMMENT '提交时权威昵称，不接受客户端伪造。',
  `MessageText` TEXT NOT NULL COMMENT '已通过Unicode校验的原始广播正文。',
  `ConsumedAt` DATETIME(6) NOT NULL COMMENT '道具扣除采用的服务端UTC时间。',
  `PublishedAt` DATETIME(6) COMMENT '世界线程完成在线投递尝试后的数据库回执时间。',
  CONSTRAINT `CK_MegaphoneMessagesChannel` CHECK (`ChannelId` IN (60, 80)),
  CONSTRAINT `CK_MegaphoneMessagesItem` CHECK (`ItemId` IN (100251, 100252, 100547, 100548, 102173, 190071, 190074, 190105)),
  CONSTRAINT `CK_MegaphoneMessagesSelection` CHECK (`SelectionId` > 0),
  CONSTRAINT `CK_MegaphoneMessagesText` CHECK (length(`MessageText`) >= 1 AND length(`MessageText`) <= 256),
  CONSTRAINT `CK_MegaphoneMessagesType` CHECK (`RequestType` IN (101, 102, 103)),
  PRIMARY KEY (`BusinessId`),
  KEY `IX_MegaphoneMessagesPending` (`ConsumedAt`),
  KEY `IX_MegaphoneMessagesPlayerRecent` (`PlayerId`, `ConsumedAt` DESC)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='520喇叭消费凭据和耐久待广播正文；重启后恢复尚未确认的投递。';

-- bf-step table ModerationAudit
CREATE TABLE `ModerationAudit` (
  `AuditId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '审计编号。',
  `Action` TEXT NOT NULL COMMENT '管理动作。',
  `AccountId` BIGINT COMMENT '关联账号。',
  `MachineCode` VARCHAR(69) COMMENT '关联设备摘要。',
  `Actor` TEXT NOT NULL COMMENT '管理员标识。',
  `Reason` TEXT NOT NULL COMMENT '操作原因。',
  `OccurredAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '操作时间。',
  `Details` JSON NOT NULL DEFAULT ('{}') COMMENT '结构化执行结果。',
  CONSTRAINT `CK_ModerationAuditActor` CHECK (char_length(`Actor`) >= 1 AND char_length(`Actor`) <= 64),
  CONSTRAINT `CK_ModerationAuditReason` CHECK (char_length(`Reason`) >= 1 AND char_length(`Reason`) <= 256),
  PRIMARY KEY (`AuditId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='管理命令审计；客户端无写入入口。';

-- bf-step table ModerationSanctions
CREATE TABLE `ModerationSanctions` (
  `SanctionId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '处罚编号。',
  `Kind` VARCHAR(191) NOT NULL COMMENT '处罚种类。',
  `AccountId` BIGINT COMMENT '账号目标；设备处罚为空。',
  `MachineCode` VARCHAR(69) COMMENT '设备目标；账号处罚为空。',
  `StartsAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '开始时间。',
  `ExpiresAt` DATETIME(6) COMMENT '到期时间；空表示永久。',
  `Reason` TEXT NOT NULL COMMENT '处罚原因。',
  `Actor` TEXT NOT NULL COMMENT '管理员标识。',
  `RevokedAt` DATETIME(6) COMMENT '撤销或替换时间。',
  `RevokedBy` TEXT COMMENT '撤销管理员。',
  `RevocationReason` TEXT COMMENT '撤销原因。',
  CONSTRAINT `CK_ModerationExpiry` CHECK (`ExpiresAt` IS NULL OR `ExpiresAt` > `StartsAt`),
  CONSTRAINT `CK_ModerationRevocation` CHECK (`RevokedAt` IS NULL AND `RevokedBy` IS NULL AND `RevocationReason` IS NULL OR `RevokedAt` IS NOT NULL AND `RevokedBy` IS NOT NULL AND `RevocationReason` IS NOT NULL AND `RevokedAt` >= `StartsAt` AND char_length(`RevokedBy`) >= 1 AND char_length(`RevokedBy`) <= 64 AND char_length(`RevocationReason`) >= 1 AND char_length(`RevocationReason`) <= 256),
  CONSTRAINT `CK_ModerationSanctionsActor` CHECK (char_length(`Actor`) >= 1 AND char_length(`Actor`) <= 64),
  CONSTRAINT `CK_ModerationSanctionsKind` CHECK (`Kind` IN ('mute', 'account_ban', 'device_ban')),
  CONSTRAINT `CK_ModerationSanctionsReason` CHECK (char_length(`Reason`) >= 1 AND char_length(`Reason`) <= 256),
  CONSTRAINT `CK_ModerationTarget` CHECK (`Kind` = 'device_ban' AND `MachineCode` IS NOT NULL AND `AccountId` IS NULL OR (`Kind` IN ('mute', 'account_ban')) AND `AccountId` IS NOT NULL AND `MachineCode` IS NULL),
  PRIMARY KEY (`SanctionId`),
  KEY `IX_ModerationAccountActive` (`AccountId`, `Kind`),
  KEY `IX_ModerationDeviceActive` (`MachineCode`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='禁言、账号封禁和设备封禁；到期或解除保留历史。';

-- bf-step table MysteryShopSelections
CREATE TABLE `MysteryShopSelections` (
  `PlayerId` BIGINT NOT NULL COMMENT '选品所属玩家，每名玩家只保留当前一代。',
  `Generation` BIGINT NOT NULL COMMENT '单调非负选品代次，用于拒绝过期客户端操作。',
  `OfferSnapshot` JSON NOT NULL COMMENT '本代固定销售规格数组，不回读后来发布的目录。',
  `ExpiresAt` DATETIME(6) NOT NULL COMMENT '本代选品过期时间。',
  `ConfigurationVersionId` BIGINT NOT NULL COMMENT '生成选品采用的已发布配置版本。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '本代选品建立时间。',
  `LegacyImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin COMMENT '旧固定选品的来源导入批次；导入时保持代次和到期时间。',
  CONSTRAINT `CK_MysterySelectionsExpiryOrder` CHECK (`ExpiresAt` > `CreatedAt`),
  CONSTRAINT `CK_MysterySelectionsGenerationNonnegative` CHECK (`Generation` >= 0),
  CONSTRAINT `CK_MysterySelectionsOffersArray` CHECK (LOWER(JSON_TYPE(`OfferSnapshot`)) = 'array'),
  PRIMARY KEY (`PlayerId`),
  KEY `IX_MysteryShopSelectionsExpiry` (`ExpiresAt`, `PlayerId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='玩家当前神秘商店固定选品；恢复时沿用同一代结果，不重新随机。';

-- bf-step table NativeMedalDefinitions
CREATE TABLE `NativeMedalDefinitions` (
  `MedalKey` INT NOT NULL COMMENT '原生 mode*10000+medalid；不允许将称号物品 ID 伪装成勋章。',
  `SourceXmlSha256` TEXT NOT NULL COMMENT '已提取原始 Config/BF_title.xml 的 SHA256，不是当前重新解包声明。',
  CONSTRAINT `CK_NativeMedalDefinitionsKey` CHECK (`MedalKey` > 0 AND (`MedalKey` % 10000) > 0),
  CONSTRAINT `CK_NativeMedalDefinitionsSource` CHECK (REGEXP_LIKE(`SourceXmlSha256`,'^[0-9a-f]{64}$','c')),
  PRIMARY KEY (`MedalKey`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='原始 BF_title.xml 的 141 个非零勋章键；目录存在不代表授予规则已经实现。';

-- bf-step table NativeMedalEvents
CREATE TABLE `NativeMedalEvents` (
  `MatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '服务端对局 UUID，且必须已成功结算；同局同玩家同勋章仅一行。',
  `PlayerId` BIGINT NOT NULL COMMENT '已冻结参赛玩家，不能由请求任意指定他人。',
  `MedalKey` INT NOT NULL COMMENT '原始勋章键，必须与已结算模式一致；非对局勋章生产者仍未实现。',
  `Increment` INT NOT NULL COMMENT '真实授勋事件增量，不是泡封击爆帮助原始计分；每局授予次数的原版规则尚未恢复。',
  `RuleEvidenceSha256` TEXT NOT NULL COMMENT '可信生产者采用的原版完整规则证据摘要；摘要字段本身不能证明规则，未安装规则不得发奖。',
  `RecordedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '服务器账本落库时间，不接受客户端时间作为授权。',
  CONSTRAINT `CK_NativeMedalEventsEvidence` CHECK (REGEXP_LIKE(`RuleEvidenceSha256`,'^[0-9a-f]{64}$','c')),
  CONSTRAINT `CK_NativeMedalEventsIncrement` CHECK (`Increment` > 0),
  PRIMARY KEY (`MatchId`, `PlayerId`, `MedalKey`),
  KEY `IX_NativeMedalEventsPlayerKey` (`PlayerId`, `MedalKey`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='只追加的已结算勋章事件；目前没有自动业务生产者，禁止将客户端成绩报告、终生战斗总数或称号库存回填为增量。';

-- bf-step table NativeTitleClaims
CREATE TABLE `NativeTitleClaims` (
  `ClaimId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '原生称号claim_id，来自认证身份、明确原表或事务内权威事实',
  `PlayerId` BIGINT NOT NULL COMMENT '原生称号player_id，来自认证身份、明确原表或事务内权威事实',
  `TitleId` INT NOT NULL COMMENT '原生称号title_id，来自认证身份、明确原表或事务内权威事实',
  `ItemInstanceId` BIGINT NOT NULL COMMENT '原生称号item_instance_id，来自认证身份、明确原表或事务内权威事实',
  `InventoryTransactionId` BIGINT NOT NULL COMMENT '原生称号inventory_transaction_id，来自认证身份、明确原表或事务内权威事实',
  `ClaimedAt` DATETIME(6) NOT NULL DEFAULT (CURRENT_TIMESTAMP(6)) COMMENT '原生称号claimed_at，来自认证身份、明确原表或事务内权威事实',
  PRIMARY KEY (`ClaimId`),
  UNIQUE KEY `UX_NativeTitleClaimInventory` (`InventoryTransactionId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='原生称号目录、穿戴或兑换不可变审计；不自动授勋或猜测空条件资格';

-- bf-step table NativeTitleDefinitions
CREATE TABLE `NativeTitleDefinitions` (
  `TitleId` INT NOT NULL COMMENT '原生称号title_id，来自认证身份、明确原表或事务内权威事实',
  `StockId` INT NOT NULL COMMENT '原生称号stock_id，来自认证身份、明确原表或事务内权威事实',
  `TitleName` TEXT NOT NULL COMMENT '原生称号title_name，来自认证身份、明确原表或事务内权威事实',
  `TitleType` INT NOT NULL COMMENT '原生称号title_type，来自认证身份、明确原表或事务内权威事实',
  `ClaimEnabled` BOOLEAN NOT NULL COMMENT '原生称号claim_enabled，来自认证身份、明确原表或事务内权威事实',
  `SourceXmlSha256` TEXT NOT NULL COMMENT '原生称号source_xml_sha256，来自认证身份、明确原表或事务内权威事实',
  CONSTRAINT `CK_NativeTitleId` CHECK (`TitleId` >= 400000 AND `TitleId` < 401000),
  CONSTRAINT `CK_NativeTitleSource` CHECK (REGEXP_LIKE(`SourceXmlSha256`,'^[0-9a-f]{64}$','c')),
  PRIMARY KEY (`TitleId`),
  UNIQUE KEY `UX_NativeTitleDefinitionStock` (`StockId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='原生称号目录、穿戴或兑换不可变审计；不自动授勋或猜测空条件资格';

-- bf-step table NativeTitleEquipment
CREATE TABLE `NativeTitleEquipment` (
  `PlayerId` BIGINT NOT NULL COMMENT '原生称号player_id，来自认证身份、明确原表或事务内权威事实',
  `ItemInstanceId` BIGINT NOT NULL COMMENT '原生称号item_instance_id，来自认证身份、明确原表或事务内权威事实',
  `TitleId` INT NOT NULL COMMENT '原生称号title_id，来自认证身份、明确原表或事务内权威事实',
  `SelectedAt` DATETIME(6) NOT NULL DEFAULT (CURRENT_TIMESTAMP(6)) COMMENT '原生称号selected_at，来自认证身份、明确原表或事务内权威事实',
  PRIMARY KEY (`PlayerId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='原生称号目录、穿戴或兑换不可变审计；不自动授勋或猜测空条件资格';

-- bf-step table NativeTitleMedalDebits
CREATE TABLE `NativeTitleMedalDebits` (
  `ClaimId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '原生称号claim_id，来自认证身份、明确原表或事务内权威事实',
  `PlayerId` BIGINT NOT NULL COMMENT '原生称号player_id，来自认证身份、明确原表或事务内权威事实',
  `MedalKey` INT NOT NULL COMMENT '原生称号medal_key，来自认证身份、明确原表或事务内权威事实',
  `Amount` BIGINT NOT NULL COMMENT '原生称号amount，来自认证身份、明确原表或事务内权威事实',
  CONSTRAINT `CK_NativeTitleDebitPositive` CHECK (`Amount` > 0),
  PRIMARY KEY (`ClaimId`, `MedalKey`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='原生称号目录、穿戴或兑换不可变审计；不自动授勋或猜测空条件资格';

-- bf-step table NativeTitleRequirements
CREATE TABLE `NativeTitleRequirements` (
  `TitleId` INT NOT NULL COMMENT '原生称号title_id，来自认证身份、明确原表或事务内权威事实',
  `MedalKey` INT NOT NULL COMMENT '原生称号medal_key，来自认证身份、明确原表或事务内权威事实',
  `RequiredCount` BIGINT NOT NULL COMMENT '原生称号required_count，来自认证身份、明确原表或事务内权威事实',
  CONSTRAINT `CK_NativeTitleRequirementAmount` CHECK (`RequiredCount` > 0 AND `RequiredCount` <= 2147483647),
  PRIMARY KEY (`TitleId`, `MedalKey`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='原生称号目录、穿戴或兑换不可变审计；不自动授勋或猜测空条件资格';

-- bf-step table NativeTitleSelectionEvents
CREATE TABLE `NativeTitleSelectionEvents` (
  `SelectionEventId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '原生称号selection_event_id，来自认证身份、明确原表或事务内权威事实',
  `PlayerId` BIGINT NOT NULL COMMENT '原生称号player_id，来自认证身份、明确原表或事务内权威事实',
  `TitleId` INT NOT NULL COMMENT '原生称号title_id，来自认证身份、明确原表或事务内权威事实',
  `ItemInstanceId` BIGINT NOT NULL COMMENT '原生称号item_instance_id，来自认证身份、明确原表或事务内权威事实',
  `ActionFlag` INT NOT NULL COMMENT '原生称号action_flag，来自认证身份、明确原表或事务内权威事实',
  `OccurredAt` DATETIME(6) NOT NULL DEFAULT (CURRENT_TIMESTAMP(6)) COMMENT '原生称号occurred_at，来自认证身份、明确原表或事务内权威事实',
  CONSTRAINT `CK_NativeTitleSelectionAction` CHECK (`ActionFlag` IN (0, 1)),
  PRIMARY KEY (`SelectionEventId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='原生称号目录、穿戴或兑换不可变审计；不自动授勋或猜测空条件资格';

-- bf-step table OperationRequests
CREATE TABLE `OperationRequests` (
  `OperationRequestId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '服务端生成的稳定操作 UUID。',
  `PlayerId` BIGINT NOT NULL COMMENT '发起操作的玩家。',
  `OperationType` VARCHAR(64) NOT NULL COMMENT '具名业务用例，例如 purchase_offer；不能使用宽泛的 do_it。',
  `IdempotencyKey` VARCHAR(128) NOT NULL COMMENT '该玩家及用例作用域内的去重键；不等同于尚未证实的客户端 C2S809 请求号。',
  `RequestDigest` LONGBLOB NOT NULL COMMENT '规范化业务参数摘要；同键不同摘要必须拒绝。',
  `OperationStatus` TEXT NOT NULL DEFAULT ('pending') COMMENT '执行状态：pending、succeeded 或 failed。',
  `ResultReference` TEXT COMMENT '成功或失败结果的稳定引用；无独立结果对象时为 NULL。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '首次受理时间。',
  `CompletedAt` DATETIME(6) COMMENT '成功或失败的完成时间；pending 为 NULL。',
  CONSTRAINT `CK_OperationRequestsCompletion` CHECK (`OperationStatus` = 'pending' AND `CompletedAt` IS NULL OR (`OperationStatus` IN ('succeeded', 'failed')) AND `CompletedAt` IS NOT NULL),
  CONSTRAINT `CK_OperationRequestsDigestNonempty` CHECK (octet_length(`RequestDigest`) > 0),
  CONSTRAINT `CK_OperationRequestsKeyNonempty` CHECK (char_length(`IdempotencyKey`) > 0),
  CONSTRAINT `CK_OperationRequestsStatus` CHECK (`OperationStatus` IN ('pending', 'succeeded', 'failed')),
  CONSTRAINT `CK_OperationRequestsTypeNonempty` CHECK (char_length(`OperationType`) > 0),
  PRIMARY KEY (`OperationRequestId`),
  UNIQUE KEY `UX_OperationRequestsPlayerKey` (`PlayerId`, `OperationType`, `IdempotencyKey`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='记录用例级稳定请求、原始参数摘要和可查询结果，支持同键重试判定。';

-- bf-step table OutboxEvents
CREATE TABLE `OutboxEvents` (
  `EventId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '全局稳定事件 UUID，也是消费者去重键。',
  `AggregateType` TEXT NOT NULL COMMENT '事件所属聚合类型，例如 match 或 purchase_order。',
  `AggregateId` TEXT NOT NULL COMMENT '聚合稳定文本身份，避免混用不同强类型数字 ID。',
  `EventType` TEXT NOT NULL COMMENT '版本化事件类型代码。',
  `Payload` JSON NOT NULL COMMENT '不含秘密的版本化 JSON 对象负载。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '事件与业务结果同事务建立的时间。',
  `PublishedAt` DATETIME(6) COMMENT '最近一次成功发布时间；尚未发布为 NULL。',
  `AttemptCount` INT NOT NULL DEFAULT 0 COMMENT '发布尝试次数，用于退避和告警。',
  `LastErrorCode` TEXT COMMENT '最近一次失败的稳定分类；不得保存连接串或正文秘密。',
  CONSTRAINT `CK_OutboxEventsAggregateIdNonempty` CHECK (char_length(`AggregateId`) > 0),
  CONSTRAINT `CK_OutboxEventsAggregateTypeNonempty` CHECK (char_length(`AggregateType`) > 0),
  CONSTRAINT `CK_OutboxEventsAttemptNonnegative` CHECK (`AttemptCount` >= 0),
  CONSTRAINT `CK_OutboxEventsPayloadObject` CHECK (LOWER(JSON_TYPE(`Payload`)) = 'object'),
  CONSTRAINT `CK_OutboxEventsPublishOrder` CHECK (`PublishedAt` IS NULL OR `PublishedAt` >= `CreatedAt`),
  CONSTRAINT `CK_OutboxEventsTypeNonempty` CHECK (char_length(`EventType`) > 0),
  PRIMARY KEY (`EventId`),
  KEY `IX_OutboxEventsUnpublished` (`CreatedAt`, `EventId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='与业务事务同提交的可靠事件箱；至少一次发布，消费者按 event_id 去重。';

-- bf-step table PlayerBlocks
CREATE TABLE `PlayerBlocks` (
  `OwnerPlayerId` BIGINT NOT NULL COMMENT '创建并可解除屏蔽的玩家。',
  `BlockedPlayerId` BIGINT NOT NULL COMMENT '被该玩家屏蔽的目标玩家。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '屏蔽关系建立的数据库时间。',
  `LegacyImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin COMMENT '旧 SQLite 屏蔽关系的来源导入批次；在线新屏蔽为 NULL。',
  `CreatedAtIsEstimated` BOOLEAN NOT NULL DEFAULT false COMMENT '旧表没有建立时间时为 true，created_at 仅记录迁移切点而非历史发生时刻。',
  CONSTRAINT `CK_PlayerBlocksDifferentPlayers` CHECK (`OwnerPlayerId` <> `BlockedPlayerId`),
  CONSTRAINT `CK_PlayerBlocksLegacyTimeProvenance` CHECK (`CreatedAtIsEstimated` = (`LegacyImportBatchId` IS NOT NULL)),
  PRIMARY KEY (`OwnerPlayerId`, `BlockedPlayerId`),
  KEY `IX_PlayerBlocksBlockedPlayer` (`BlockedPlayerId`, `OwnerPlayerId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='玩家主动维护的单向屏蔽关系；业务判断双方任一方向屏蔽时禁止互动。';

-- bf-step table PlayerChatBlocks
CREATE TABLE `PlayerChatBlocks` (
  `OwnerPlayerId` BIGINT NOT NULL COMMENT '从认证连接取得的名单所有者。',
  `BlockedPlayerId` BIGINT NOT NULL COMMENT '屏蔽目标的稳定玩家身份，昵称在读取时关联当前资料。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '用于保持名单显示顺序。',
  PRIMARY KEY (`OwnerPlayerId`, `BlockedPlayerId`),
  KEY `IX_PlayerChatBlocksTarget` (`BlockedPlayerId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='个人聊天屏蔽名单；不影响好友关系、邀请或好友黑名单。';

-- bf-step table PlayerEquipment
CREATE TABLE `PlayerEquipment` (
  `PlayerId` BIGINT NOT NULL COMMENT '配装所属玩家，并参与本人库存复合外键。',
  `LoadoutId` INT NOT NULL COMMENT '主武器 primary、手雷 grenade、水晶 crystal 按零至二号页独立；手枪 secondary、锤子 melee 及其余装备共享零号页。',
  `SlotCode` VARCHAR(64) NOT NULL COMMENT '领域层验证后的装备槽代码。',
  `ItemInstanceId` BIGINT NOT NULL COMMENT '已装备的本人库存实例。',
  `EquippedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '装备变更事务提交时间。',
  CONSTRAINT `CK_PlayerEquipmentLoadoutNonnegative` CHECK (`LoadoutId` >= 0),
  CONSTRAINT `CK_PlayerEquipmentSlotLength` CHECK (char_length(`SlotCode`) >= 1 AND char_length(`SlotCode`) <= 64),
  PRIMARY KEY (`PlayerId`, `LoadoutId`, `SlotCode`),
  UNIQUE KEY `UX_PlayerEquipmentLoadoutItem` (`PlayerId`, `LoadoutId`, `ItemInstanceId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='玩家配装槽到本人库存实例的映射；同一槽位至多装备一个实例。';

-- bf-step table PlayerGiftItems
CREATE TABLE `PlayerGiftItems` (
  `GiftId` BIGINT NOT NULL COMMENT '已扣款礼物编号，和礼物主记录在同一事务建立。',
  `Ordinal` INT NOT NULL COMMENT '冻结的原版部件顺序，单品固定为一。',
  `SourceStockId` BIGINT NOT NULL COMMENT '冻结的单品来源销售编号，不在领取时依赖该销售项。',
  `ItemId` BIGINT NOT NULL COMMENT '冻结的真实库存模板，禁止用虚构套装模板发放。',
  `GrantQuantity` BIGINT NOT NULL COMMENT '冻结的数量增量，不能因后来修改商品而改变。',
  `DurationSeconds` BIGINT COMMENT '冻结的有效期增量，领取事务开始计时，空值为永久或数量堆叠。',
  `StackPolicy` TEXT COMMENT '新礼物成交时的库存策略；旧单品历史未知保持空值，领取按当前模板校验。',
  `DurationPolicy` TEXT COMMENT '新礼物成交时的期限策略；旧单品历史未知保持空值，领取按当前模板校验。',
  `MaxQuantity` BIGINT COMMENT '新礼物冻结的库存上限；旧单品历史上限未知为空，绝不把停用时零上限冻结为永久阻塞。',
  `MaxDurationSeconds` BIGINT COMMENT '新礼物冻结的累计期限上限；旧单品历史上限未知为空，领取按当前模板校验。',
  `SnapshotKind` TEXT NOT NULL DEFAULT ('frozen') COMMENT 'frozen为完整新成交快照；legacy_single只保留旧主记录的已知单品数量期限，领取使用当前模板规则。',
  CONSTRAINT `CK_PlayerGiftItemsDuration` CHECK (`DurationSeconds` IS NULL OR `DurationSeconds` > 0),
  CONSTRAINT `CK_PlayerGiftItemsKind` CHECK (`SnapshotKind` = 'frozen' AND `StackPolicy` IS NOT NULL AND `DurationPolicy` IS NOT NULL AND `MaxQuantity` IS NOT NULL OR `SnapshotKind` = 'legacy_single' AND `Ordinal` = 1 AND `SourceStockId` < 2000000 AND `StackPolicy` IS NULL AND `DurationPolicy` IS NULL AND `MaxQuantity` IS NULL AND `MaxDurationSeconds` IS NULL),
  CONSTRAINT `CK_PlayerGiftItemsMaxDuration` CHECK (`MaxDurationSeconds` IS NULL OR `MaxDurationSeconds` > 0),
  CONSTRAINT `CK_PlayerGiftItemsOrdinal` CHECK (`Ordinal` >= 1 AND `Ordinal` <= 10),
  CONSTRAINT `CK_PlayerGiftItemsPolicy` CHECK (`DurationPolicy` IN ('permanent', 'expiring', 'offer_defined')),
  CONSTRAINT `CK_PlayerGiftItemsQuantity` CHECK (`GrantQuantity` >= 1 AND `GrantQuantity` <= 2147483647 AND (`MaxQuantity` IS NULL OR `MaxQuantity` >= `GrantQuantity` AND `MaxQuantity` <= 2147483647)),
  CONSTRAINT `CK_PlayerGiftItemsSource` CHECK (`SourceStockId` >= 1 AND `SourceStockId` <= 4294967295),
  CONSTRAINT `CK_PlayerGiftItemsStack` CHECK (`StackPolicy` IN ('merge_quantity', 'single_instance')),
  PRIMARY KEY (`GiftId`, `Ordinal`),
  UNIQUE KEY `UX_PlayerGiftItemsTemplate` (`GiftId`, `ItemId`),
  KEY `IX_PlayerGiftItemsTemplate` (`ItemId`, `GiftId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='赠送成交时冻结的全套有序库存规格；列表继续使用原版单条礼物线格式。';

-- bf-step table PlayerGifts
CREATE TABLE `PlayerGifts` (
  `GiftId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '原生 DWORD 礼物编号。',
  `BusinessId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '赠送操作 UUID；关联付款和领取审计。',
  `SenderId` BIGINT NOT NULL COMMENT '经过主网关认证的赠送者。',
  `RecipientId` BIGINT NOT NULL COMMENT '按权威昵称解析的收件者。',
  `StockId` BIGINT NOT NULL COMMENT '成交时的销售规格编号。',
  `ItemId` BIGINT NOT NULL COMMENT '冻结的发放模板。',
  `Quantity` BIGINT NOT NULL COMMENT '冻结的发放数量。',
  `DurationSeconds` BIGINT COMMENT '领取时开始计算的有效秒数；空值表示永久。',
  `CurrencyCode` VARCHAR(191) NOT NULL COMMENT '实际扣款币种。',
  `Price` BIGINT NOT NULL COMMENT '实际扣款金额。',
  `Message` TEXT NOT NULL DEFAULT ('') COMMENT '赠送者提交的有界留言。',
  `Thanks` TEXT NOT NULL DEFAULT ('') COMMENT '收件者领取后提交的有界感谢留言。',
  `ThankKind` SMALLINT NOT NULL DEFAULT 0 COMMENT '原生感谢表情类型；零未感谢，二至五为允许的表情。',
  `GiftState` SMALLINT NOT NULL DEFAULT 0 COMMENT '零待领取、一已领取、二已丢弃；终态不再发放。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '成功扣款并建立礼物的时间。',
  `ProcessedAt` DATETIME(6) COMMENT '领取或明确丢弃的事务时间。',
  `SenderHidden` BOOLEAN NOT NULL DEFAULT false COMMENT '发送者隐藏历史记录，不改变礼物资产状态。',
  `RecipientHidden` BOOLEAN NOT NULL DEFAULT false COMMENT '收件者隐藏已处理历史，不允许隐藏待领取资产。',
  `ComponentCount` INT NOT NULL DEFAULT 1 COMMENT '成交时冻结的部件数；领取必须核对完整快照，缺件不得部分发放。',
  CONSTRAINT `CK_PlayerGiftsComponents` CHECK (`ComponentCount` >= 1 AND `ComponentCount` <= 10),
  CONSTRAINT `CK_PlayerGiftsDuration` CHECK (`DurationSeconds` IS NULL OR `DurationSeconds` > 0),
  CONSTRAINT `CK_PlayerGiftsHidden` CHECK (NOT `RecipientHidden` OR `GiftState` <> 0),
  CONSTRAINT `CK_PlayerGiftsParties` CHECK (`SenderId` <> `RecipientId`),
  CONSTRAINT `CK_PlayerGiftsPrice` CHECK (`Price` >= 0 AND (`CurrencyCode` IN ('lucci', 'bubble_cash'))),
  CONSTRAINT `CK_PlayerGiftsProcessed` CHECK ((`GiftState` = 0) = (`ProcessedAt` IS NULL)),
  CONSTRAINT `CK_PlayerGiftsQuantity` CHECK (`Quantity` >= 1 AND `Quantity` <= 2147483647),
  CONSTRAINT `CK_PlayerGiftsState` CHECK (`GiftState` IN (0, 1, 2)),
  CONSTRAINT `CK_PlayerGiftsText` CHECK (char_length(`Message`) <= 256 AND char_length(`Thanks`) <= 256),
  CONSTRAINT `CK_PlayerGiftsThank` CHECK ((`ThankKind` IN (0, 2, 3, 4, 5)) AND (`ThankKind` = 0 OR `GiftState` = 1)),
  PRIMARY KEY (`GiftId`),
  UNIQUE KEY `UX_PlayerGiftsBusiness` (`BusinessId`),
  KEY `IX_PlayerGiftsRecipient` (`RecipientId`, `GiftId` DESC),
  KEY `IX_PlayerGiftsSender` (`SenderId`, `GiftId` DESC)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='已扣款礼物及不可变成交规格；领取与库存发放必须同事务提交。';

-- bf-step table PlayerItemAttributes
CREATE TABLE `PlayerItemAttributes` (
  `AttributeId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '不可变属性记录主键，保留实例删除前的属性历史。',
  `ItemInstanceId` BIGINT COMMENT '当前所属库存实例；实例删除后清空且不可重新绑定。',
  `PlayerId` BIGINT NOT NULL COMMENT '原实例所属玩家，实例删除后保留且不可转移。',
  `SourceItemId` BIGINT NOT NULL COMMENT '原属性首DWORD的物品模板键，必须与真实实例一致，不是SQL实例号。',
  `DefinitionId` INT NOT NULL COMMENT '原ItemAttr定义，当前只恢复100号高级化。',
  `AttributeValues` JSON NOT NULL COMMENT '原有符号整数向量；100号固定为2、1、1，顺序不可改变。',
  `Category` INT NOT NULL COMMENT '原属性分类，100号固定为100。',
  `Label` TEXT NOT NULL COMMENT '原属性文字，当前100号为空。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '该属性记录首次持久化时间。',
  CONSTRAINT `CK_ItemAttributeCategory` CHECK (`Category` = 100),
  CONSTRAINT `CK_ItemAttributeDefinition` CHECK (`DefinitionId` = 100),
  CONSTRAINT `CK_ItemAttributeLabel` CHECK (`Label` = ''),
  CONSTRAINT `CK_ItemAttributeSource` CHECK (`SourceItemId` IN (
    100260,100261,100262,100263,100264,100358,100359,100360,100361,100362,100363,100364,
    100365,100366,100367,100413,100414,100415,100416,100417,100418,100419,100420,100421,
    100422,100426,100427,100428,100429,100430,100433,100434,100435,100436,100437,100444,
    100445,100446,100447,100448,100449,100450,100451,100452,100453,100455,100456,100457,
    100458,100459,100501,100502,100503,100504,100505,100510,100511,100512,100513,100514,
    100515,100516,100517,100518,100519,100530,100531,100532,100533,100534,100541,100542,
    100543,100544,100545,100553,100554,100555,100556,100557,100566,100567,100568,100569,
    100570,100571,100572,100573,100574,100575,100579,100580,100581,100582,100583,100584,
    100585,100586,100587,100588,100606,100607,100608,100609,100610,100612,100613,100614,
    100615,100616,100617,100618,100619,100630,100631,100632,100633,100634,100635,100636,
    100637,100638,100639,100645,100646,100647,100648,100649,100658,100659,100660,100661,
    100662,100663,100664,100665,100666,100667,100680,100681,100682,100683,100684,100685,
    100686,100687,100688,100689,100690,100691,100692,100693,100694,100702,100703,100704,
    100705,100706,100707,100708,100709,100710,100711,100717,100718,100719,100720,100721,
    100722,100723,100724,100725,100726,100729,100730,100731,100732,100733,100734,100735,
    100736,100737,100738,100742,100743,100744,100745,100746,100747,100748,100749,100750,
    100751,100752,100753,100754,100755,100756,100757,100758,100759,100760,100761,100762,
    100763,100764,100765,100766,100767,100768,100769,100770,100771,100772,100773,100785,
    100786,100787,100788,100789,100790,100791,100792,100793,100794,100795,100796,100797,
    100798,100799,100800,100801,100802,100803,100804,100805,100806,100807,100808,100809,
    100810,100811,100812,100813,100814,100825,100826,100827,100828,100829,100830,100831,
    100832,100833,100834,100835,100836,100837,100838,100839,100840,100841,100842,100843,
    100844,100847,100848,100849,100850,100851,100852,100853,100854,100855,100856,100857,
    100858,100859,100860,100861,100884,100885,100886,100887,100888,100889,100890,100891,
    100892,100893,100894,100895,100896,100897,100898,100900,100901,100902,100903,100904,
    100918,100919,100920,100921,100922,100941,100942,100943,100944,100945,100946,100947,
    100948,100949,100950,100956,100957,100958,100959,100960,100966,100967,100968,100969,
    100970,100971,100972,100973,100974,100975,100979,100980,100981,100982,100983,100987,
    100988,100989,100991,100992,100993,100994,100995,100996,100997,100998,100999,101000,
    101002,101003,101004,101005,101006,101007,101008,101009,101010,101011,101012,101013,
    101014,101015,101016,101017,101018,101019,101020,101021,101023,101024,101025,101026,
    101027,101029,101030,101031,101032,101033,101036,101037,101038,101039,101040,101052,
    101053,101054,101055,101056,101070,101071,101072,101073,101074,101076,101077,101078,
    101079,101080,101087,101088,101089,101090,101091,101095,101096,101097,101098,101099,
    101101,101102,101103,101104,101105,101106,101107,101108,101109,101110,101156,101157,
    101158,101159,101160,101170,101171,101172,101173,101174,101175,101176,101177,101178,
    101179,101186,101187,101188,101189,101190,101191,101192,101193,101194,101195,101196,
    101197,101198,101199,101200,101201,101202,101203,101204,101205,101214,101215,101216,
    101217,101218,101235,101236,101237,101238,101239,101321,101322,101323,101324,101325,
    101331,101332,101333,101334,101335,101345,101346,101347,101348,101349,101357,101358,
    101359,101360,101361,101384,101385,101386,101387,101388,101389,101390,101391,101392,
    101393,101407,101408,101409,101410,101411,101417,101418,101419,101420,101421,101437,
    101438,101439,101440,101441,101456,101457,101458,101459,101460,101466,101467,101468,
    101469,101470,101480,101481,101482,101483,101484,101490,101491,101510,101511,101514,
    101515,101518,101519,101522,101523,101524,101525,101526,101532,101542,101543,101544,
    101545,101546,101570,101571,101572,101573,101574,101585,101586,101587,101588,101589,
    101608,101609,101610,101611,101612,101619,101620,101621,101622,101623,101635,101636,
    101688,101689,101690,101691,101692,101698,101699,101705,101706,101707,101708,101709,
    101727,101728,101729,101730,101731,101732,101733,101734,101735,101736,101737,101738,
    101751,101752,101775,101776,101777,101778,101779,101795,101796,101797,101798,101799,
    101805,101806,101807,101808,101809,101810,101811,101812,101813,101814,101828,101829,
    101832,101833,101834,101835,101836,101842,101843,101844,101845,101846,101871,101872,
    101875,101876,101877,101878,101879,101885,101886,101889,101890,101891,101892,101893,
    101911,101925,101926,101927,101928,101929,101930,101931,101932,101933,101934,101935,
    101936,101939,101940,101950,101951,101952,101953,101954,101964,101965,101966,101967,
    101968,101983,101984,102000,102001,102002,102003,102004,102012,102013,102018,102019,
    102020,102021,102022,102024,102025,102035,102036,102037,102038,102039,102052,102053,
    102054,102055,102056,102058,102059,102060,102061,102062,102074,102075,102076,102077,
    102078,102084,102085,102093,102094,102101,102102,102109,102110,102111,102112,102113,
    102117,102118,102119,102120,102121,102124,102125,102126,102127,102128,102129,102130,
    102143,102144,102178,102179,102180,102181,102182,102188,102189,102192,102193,102194,
    102195,102196,102199,102200,102201,102202,102203,102207,102208,102209,102210,102211,
    102272,102273,102275,102276,102277,102278,102279,102307,102308,102309,102310,102339,
    102340,102341,102342,102343,102344,102345,102346,102347,102348,102349,102350,102356,
    102357,102370,102371,102442,102443,110075,110076,110077,110078,110079,110080,110081,
    110082,110083,110084,110085,110086,110087,110088,110089,110090,110091,110092,110093,
    110094,110096,110097,110098,110099,110100,190054,190055,190056,190057,190058,190059,
    190060,190061,190062,190063,190064,190065,190076,190114,190115
  )),
  CONSTRAINT `CK_ItemAttributeValues` CHECK (`AttributeValues` = JSON_ARRAY(2, 1, 1)),
  PRIMARY KEY (`AttributeId`),
  UNIQUE KEY `UX_ItemAttributeInstance` (`ItemInstanceId`, `DefinitionId`),
  KEY `IX_ItemAttributePlayer` (`PlayerId`, `ItemInstanceId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='经验证的原库存动态属性；绑定精确实例，删除库存仅解除引用并保留历史，尚不代表已实现升级发放。';

-- bf-step table PlayerItems
CREATE TABLE `PlayerItems` (
  `ItemInstanceId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '库存实例主键；迁移时可显式保留有效旧 instance_id。',
  `PlayerId` BIGINT NOT NULL COMMENT '持有该物品的玩家。',
  `ItemId` BIGINT NOT NULL COMMENT '被持有的物品模板编号。',
  `Quantity` BIGINT NOT NULL COMMENT '普通库存为正整数数量；原始Life模板此字段为剩余Life，允许零但不可装备使用。',
  `AcquiredAt` DATETIME(6) NOT NULL COMMENT '首次获得或建立迁移期初项的时间。',
  `ExpiresAt` DATETIME(6) COMMENT '绝对到期时间；NULL 统一表示永久。',
  `SourceCode` TEXT NOT NULL COMMENT '获得来源代码，例如 purchase、starter 或 legacy_import。',
  `LegacyImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin COMMENT '旧库存规范化记录的来源导入批次；在线新资产为 NULL。',
  CONSTRAINT `CK_PlayerItemsExpiryOrder` CHECK (`ExpiresAt` IS NULL OR `ExpiresAt` > `AcquiredAt`),
  CONSTRAINT `CK_PlayerItemsQuantityPositive` CHECK (`Quantity` > 0 OR `Quantity` = 0 AND `ExpiresAt` IS NULL AND `ItemId` IN (
  101447,101448,101449,101450,101451,101452,101453,101454,101455,101700,
  101701,101702,101703,101704,101786,101856,101862,101920,101922,101956,
  101987,101999,102029,102068,102069,102070,102073,102089,102115,102147,
  102265,102314,102315,102316,102317,102318,102369,190149,190150,190155,
  190157,190175,190202,190244,201337,201350,201359,201362,201923,201924,
  202339,202386,202394,202400,202924,202938,202939,202956,203381,203390,
  203395,203925,203926,203934,203935,203949,204131,204375,204382,204388,
  204924,204933,204934,204944,207925,207928,207941,207946,207952,207975
 )),
  CONSTRAINT `CK_PlayerItemsSourceNonempty` CHECK (char_length(`SourceCode`) > 0),
  PRIMARY KEY (`ItemInstanceId`),
  UNIQUE KEY `UX_PlayerItemsInstanceOwner` (`ItemInstanceId`, `PlayerId`),
  UNIQUE KEY `UX_PlayerItemsPlayerItem` (`PlayerId`, `ItemId`),
  KEY `IX_PlayerItemsPlayerExpiry` (`PlayerId`, `ExpiresAt`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='玩家当前库存合并项；首版保留旧系统每玩家每 item 一行的语义。';

-- bf-step table PlayerKeyBindings
CREATE TABLE `PlayerKeyBindings` (
  `PlayerId` BIGINT NOT NULL COMMENT '当前认证玩家；数据包不含或接受其他玩家编号。',
  `ActionCode` VARCHAR(32) NOT NULL COMMENT 'KeyMapConfig.csv 中可修改的 codeName；应用层严格白名单。',
  `KeyCode` INT COMMENT '原2016键码：正数1..255为Windows虚拟键；NULL为明确未绑定，S2C981写DWORD0。缺少整行动作使用客户端默认，不能把未绑定动作删成默认。',
  CONSTRAINT `CK_PlayerKeyBindingsAction` CHECK (char_length(`ActionCode`) >= 1 AND char_length(`ActionCode`) <= 32),
  CONSTRAINT `CK_PlayerKeyBindingsKey` CHECK (`KeyCode` >= 1 AND `KeyCode` <= 255),
  PRIMARY KEY (`PlayerId`, `ActionCode`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='原版 C2S2016 保存的玩家按键覆盖表；空表使用客户端默认，S2C981 重载。';

-- bf-step table PlayerModeStatistics
CREATE TABLE `PlayerModeStatistics` (
  `PlayerId` BIGINT NOT NULL COMMENT '战绩所属玩家。',
  `ModeId` INT NOT NULL COMMENT '协议映射后的模式编号。',
  `Matches` BIGINT NOT NULL DEFAULT 0 COMMENT '累计完成场次。',
  `Wins` BIGINT NOT NULL DEFAULT 0 COMMENT '累计胜场。',
  `Losses` BIGINT NOT NULL DEFAULT 0 COMMENT '累计负场。',
  `Draws` BIGINT NOT NULL DEFAULT 0 COMMENT '累计平局场次。',
  `Bubbles` BIGINT NOT NULL DEFAULT 0 COMMENT '累计泡封次数。',
  `Pangs` BIGINT NOT NULL DEFAULT 0 COMMENT '累计击爆次数。',
  `Deaths` BIGINT NOT NULL DEFAULT 0 COMMENT '累计死亡或被击爆次数，按旧统计规则导入。',
  `Helps` BIGINT NOT NULL DEFAULT 0 COMMENT '累计助攻次数。',
  `PlayedSeconds` BIGINT NOT NULL DEFAULT 0 COMMENT '累计有效对局秒数。',
  `ObjectivePoints` BIGINT NOT NULL DEFAULT 0 COMMENT '累计模式目标积分。',
  CONSTRAINT `CK_PlayerModeStatisticsCountsNonnegative` CHECK (`Matches` >= 0 AND `Wins` >= 0 AND `Losses` >= 0 AND `Draws` >= 0 AND `Bubbles` >= 0 AND `Pangs` >= 0 AND `Deaths` >= 0 AND `Helps` >= 0 AND `PlayedSeconds` >= 0 AND `ObjectivePoints` >= 0),
  CONSTRAINT `CK_PlayerModeStatisticsModeNonnegative` CHECK (`ModeId` >= 0),
  CONSTRAINT `CK_PlayerModeStatisticsOutcomes` CHECK ((`Wins` + `Losses` + `Draws`) <= `Matches`),
  PRIMARY KEY (`PlayerId`, `ModeId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='按玩家和模式保存当前累计战绩；未知旧字段不在迁移时猜测。';

-- bf-step table PlayerPetGrowth
CREATE TABLE `PlayerPetGrowth` (
  `PetGrowthId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '成长记录主键；宠物被消耗后仍保留原记录，不以级联删除清除业务历史。',
  `ItemInstanceId` BIGINT COMMENT '所属宠物库存实例；删除实例时清空引用，重新取得不会继承已删除实例的成长。',
  `PlayerId` BIGINT NOT NULL COMMENT '成长记录所属玩家；实例被消耗后保留历史主人。',
  `PetId` BIGINT NOT NULL COMMENT '宠物模板编号；实例被消耗后仍可识别原宠物类型。',
  `Level` INT NOT NULL DEFAULT 0 COMMENT '内部升级次数，零为显示第一阶段；具体宠物的上限由已验证目录在写事务复核。',
  `Experience` INT NOT NULL DEFAULT 0 COMMENT '当前阶段经验，达到一百后可单独请求升阶段；最大阶段不得继续扣粮。',
  `FedUntil` DATETIME(6) COMMENT '普通及中级粮下次允许喂食的 UTC 时间；高级粮可绕过冷却但仍检查阶段和经验。',
  `UpdatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '最近一次喂食或升阶段的服务端时间。',
  CONSTRAINT `CK_PlayerPetGrowthExperience` CHECK (`Experience` >= 0 AND `Experience` <= 100),
  CONSTRAINT `CK_PlayerPetGrowthLevel` CHECK (`Level` >= 0 AND `Level` <= 2),
  PRIMARY KEY (`PetGrowthId`),
  UNIQUE KEY `UX_PlayerPetGrowthItem` (`ItemInstanceId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='已喂养宠物的持久化阶段、经验与喂食冷却；不存在记录的宠物视为第一阶段零经验。';

-- bf-step table PlayerProfiles
CREATE TABLE `PlayerProfiles` (
  `PlayerId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '玩家强类型主键；初次迁移可与旧 uid 同值，但与 account_id 语义独立。',
  `AccountId` BIGINT NOT NULL COMMENT '唯一关联的登录账号。',
  `Nickname` TEXT NOT NULL COMMENT '客户端显示昵称；未创建角色时保留迁移显示值但不参与正式唯一键。',
  `NicknameKey` VARCHAR(64) COMMENT '完成角色后按冻结规则生成的昵称唯一键；未创建时为 NULL。',
  `IsCharacterCreated` BOOLEAN NOT NULL DEFAULT false COMMENT '是否已原子完成昵称与初始角色创建。',
  `InitialFighterId` INT COMMENT '原版初始角色编号 1、2、11 或 12；未创建角色时为 NULL。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '玩家资料建立时间。',
  `ColorCode` INT NOT NULL DEFAULT 0 COMMENT '原版喷剂颜色编码，零为默认；由服务端目录验证后与喷剂消耗在同一事务提交。',
  `ActiveWeaponPreset` INT NOT NULL DEFAULT 0 COMMENT '选择的武器页：零为基本武器，一和二为设置页；扩张卡失效时读取回退零，保留历史配置。',
  `InitializedWeaponPresets` INT NOT NULL DEFAULT 1 COMMENT '三页访问位图；切换只选择页面，不复制基本页的主武器、手雷或水晶。',
  `LevelIconType` SMALLINT NOT NULL DEFAULT 0 COMMENT '原生等级图标偏好：0普通等级、1天梯、2竞技、3家族；阶级业务未开放时不允许选择1/2，离开家族后显示回退普通等级。',
  CONSTRAINT `CK_PlayerProfilesCharacterFields` CHECK (`IsCharacterCreated` AND `NicknameKey` IS NOT NULL AND (`InitialFighterId` IN (1, 2, 11, 12)) OR NOT `IsCharacterCreated` AND `NicknameKey` IS NULL AND `InitialFighterId` IS NULL),
  CONSTRAINT `CK_PlayerProfilesColorCode` CHECK (`ColorCode` >= 0 AND `ColorCode` <= 65535),
  CONSTRAINT `CK_PlayerProfilesInitializedPresets` CHECK (`InitializedWeaponPresets` >= 1 AND `InitializedWeaponPresets` <= 7 AND (`InitializedWeaponPresets` & 1) = 1),
  CONSTRAINT `CK_PlayerProfilesLevelIconType` CHECK (`LevelIconType` >= 0 AND `LevelIconType` <= 3),
  CONSTRAINT `CK_PlayerProfilesNicknameKeyLength` CHECK (`NicknameKey` IS NULL OR char_length(`NicknameKey`) >= 1 AND char_length(`NicknameKey`) <= 64),
  CONSTRAINT `CK_PlayerProfilesNicknameLength` CHECK (char_length(`Nickname`) >= 1 AND char_length(`Nickname`) <= 32),
  CONSTRAINT `CK_PlayerProfilesWeaponPreset` CHECK (`ActiveWeaponPreset` >= 0 AND `ActiveWeaponPreset` <= 2),
  PRIMARY KEY (`PlayerId`),
  UNIQUE KEY `UX_PlayerProfilesAccount` (`AccountId`),
  UNIQUE KEY `UX_PlayerProfilesNicknameKey` (`NicknameKey`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='一账号一份的玩家资料及首次角色创建状态。';

-- bf-step table PlayerProgressions
CREATE TABLE `PlayerProgressions` (
  `PlayerId` BIGINT NOT NULL COMMENT '玩家主键，同时保证一名玩家只有一行当前进度。',
  `Experience` BIGINT NOT NULL DEFAULT 0 COMMENT '累计经验整数，不得为负。',
  `Rating` INT NOT NULL DEFAULT 1000 COMMENT '当前匹配评级整数，不得为负。',
  `RulesVersionId` BIGINT NOT NULL COMMENT '当前进度解释所依据的已归档规则版本。',
  `UpdatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '最近一次进度提交时间。',
  CONSTRAINT `CK_PlayerProgressionsExperienceNonnegative` CHECK (`Experience` >= 0),
  CONSTRAINT `CK_PlayerProgressionsRatingNonnegative` CHECK (`Rating` >= 0),
  PRIMARY KEY (`PlayerId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='保存玩家当前经验、评级及生成这些数值的规则版本。';

-- bf-step table PlayerWallets
CREATE TABLE `PlayerWallets` (
  `PlayerId` BIGINT NOT NULL COMMENT '钱包所属玩家。',
  `CurrencyCode` VARCHAR(191) NOT NULL COMMENT '资产代码：lucci、bubble_cash 或 treasure_bubble；两种 bubble 不得合并。',
  `Balance` BIGINT NOT NULL COMMENT '当前整数余额，不得为负。',
  `UpdatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '最近一次余额事务提交时间。',
  CONSTRAINT `CK_PlayerWalletsBalanceNonnegative` CHECK (`Balance` >= 0),
  CONSTRAINT `CK_PlayerWalletsCurrency` CHECK (`CurrencyCode` IN ('lucci', 'bubble_cash', 'treasure_bubble')),
  PRIMARY KEY (`PlayerId`, `CurrencyCode`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='保存玩家每种整数资源的当前余额；所有变更必须与不可变流水同事务提交。';

-- bf-step table PrivateMessages
CREATE TABLE `PrivateMessages` (
  `MessageId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '新服务生成的递增消息游标。',
  `SenderPlayerId` BIGINT NOT NULL COMMENT '由已认证会话确定的发送玩家，不接受客户端冒充。',
  `RecipientPlayerId` BIGINT NOT NULL COMMENT '接收私聊的好友玩家。',
  `MessageBody` TEXT NOT NULL COMMENT '非空私聊正文；应用层另校验 UTF-16 上限和控制字符。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '消息事务提交时间，用于游标分页。',
  `LegacyMessageId` BIGINT COMMENT '旧 SQLite private_messages.id；新消息为 NULL。',
  `LegacyImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin COMMENT '旧 SQLite 私信的来源导入批次；在线新私信为 NULL。',
  CONSTRAINT `CK_PrivateMessagesBodyLength` CHECK (char_length(`MessageBody`) >= 1 AND char_length(`MessageBody`) <= 256),
  CONSTRAINT `CK_PrivateMessagesDifferentPlayers` CHECK (`SenderPlayerId` <> `RecipientPlayerId`),
  PRIMARY KEY (`MessageId`),
  UNIQUE KEY `UX_PrivateMessagesLegacy` (`LegacyMessageId`),
  KEY `IX_PrivateMessagesRecipientCursor` (`RecipientPlayerId`, `CreatedAt`, `MessageId`),
  KEY `IX_PrivateMessagesSenderCursor` (`SenderPlayerId`, `CreatedAt`, `MessageId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='好友之间的私聊历史；正文属于隐私数据，不写入普通文本日志。';

-- bf-step table PurchaseOrderItems
CREATE TABLE `PurchaseOrderItems` (
  `OrderItemId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '订单行递增主键。',
  `OrderId` BIGINT NOT NULL COMMENT '所属购买订单。',
  `LineNumber` INT NOT NULL COMMENT '订单内从一开始的稳定行号。',
  `StockId` BIGINT COMMENT '成交销售规格编号；旧记录缺失时为 NULL 且在快照标明 legacy unknown。',
  `ItemId` BIGINT NOT NULL COMMENT '实际发放的物品模板编号。',
  `Quantity` BIGINT NOT NULL COMMENT '本订单行发放的正整数数量。',
  `UnitPrice` BIGINT NOT NULL COMMENT '成交时每件整数价格，不回读当前商品价。',
  `DurationSeconds` BIGINT COMMENT '成交期限秒数；永久为 NULL。',
  `OfferSnapshot` JSON NOT NULL COMMENT '成交时销售规格及来源版本快照；缺失旧字段明确标记而不猜值。',
  `LegacyStockId` BIGINT COMMENT '旧 SQLite 订单记录的原始 stock 编号；即使该重复规格未进入规范在线目录也原样保留。',
  CONSTRAINT `CK_PurchaseOrderItemsDurationPositive` CHECK (`DurationSeconds` IS NULL OR `DurationSeconds` > 0),
  CONSTRAINT `CK_PurchaseOrderItemsLegacyStockPositive` CHECK (`LegacyStockId` IS NULL OR `LegacyStockId` > 0),
  CONSTRAINT `CK_PurchaseOrderItemsLinePositive` CHECK (`LineNumber` > 0),
  CONSTRAINT `CK_PurchaseOrderItemsPriceNonnegative` CHECK (`UnitPrice` >= 0),
  CONSTRAINT `CK_PurchaseOrderItemsQuantityPositive` CHECK (`Quantity` > 0),
  CONSTRAINT `CK_PurchaseOrderItemsSnapshotObject` CHECK (LOWER(JSON_TYPE(`OfferSnapshot`)) = 'object'),
  PRIMARY KEY (`OrderItemId`),
  UNIQUE KEY `UX_PurchaseOrderItemsLine` (`OrderId`, `LineNumber`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='保存每个订单行的成交物品、价格、期限和不可变销售规格快照。';

-- bf-step table PurchaseOrders
CREATE TABLE `PurchaseOrders` (
  `OrderId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '订单递增主键；服务端新订单与旧 SQLite id 分开。',
  `PlayerId` BIGINT NOT NULL COMMENT '下单玩家。',
  `OperationRequestId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '唯一关联的用例请求，用于提交结果未知后的状态查询。',
  `OrderStatus` TEXT NOT NULL DEFAULT ('pending') COMMENT '订单状态：pending、settled 或 rejected。',
  `CurrencyCode` VARCHAR(191) NOT NULL COMMENT '本单实际扣款币种。',
  `TotalAmount` BIGINT NOT NULL COMMENT '订单实际成交总额，整数且不得为负。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '首次受理订单时间。',
  `SettledAt` DATETIME(6) COMMENT '订单成功结算或明确拒绝时间；pending 为 NULL。',
  `LegacyOrderId` BIGINT COMMENT '旧 SQLite purchases.id；新订单为 NULL，未知 stock 不据此猜测。',
  `LegacyImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin COMMENT '旧购买历史的来源导入批次；用于精确重放和对账。',
  CONSTRAINT `CK_PurchaseOrdersAmountNonnegative` CHECK (`TotalAmount` >= 0),
  CONSTRAINT `CK_PurchaseOrdersCurrency` CHECK (`CurrencyCode` IN ('lucci', 'bubble_cash', 'treasure_bubble')),
  CONSTRAINT `CK_PurchaseOrdersSettlement` CHECK (`OrderStatus` = 'pending' AND `SettledAt` IS NULL OR (`OrderStatus` IN ('settled', 'rejected')) AND `SettledAt` IS NOT NULL),
  CONSTRAINT `CK_PurchaseOrdersStatus` CHECK (`OrderStatus` IN ('pending', 'settled', 'rejected')),
  PRIMARY KEY (`OrderId`),
  UNIQUE KEY `UX_PurchaseOrdersLegacy` (`LegacyOrderId`),
  UNIQUE KEY `UX_PurchaseOrdersOperation` (`OperationRequestId`),
  KEY `IX_PurchaseOrdersPlayerCreated` (`PlayerId`, `CreatedAt`, `OrderId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='购买订单头；扣款、发货、订单、操作结果和资产流水在同一事务提交。';

-- bf-step table RoomInstances
CREATE TABLE `RoomInstances` (
  `InstanceId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '随机房间实例编号，不得因房间号复用而复用。',
  `NodeId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '启动并监管此进程的节点实例。',
  `RoomId` INT NOT NULL COMMENT '现有客户端可表示的房间号。',
  `OwnerEpoch` BIGINT NOT NULL COMMENT '正所有权代次；续租和写入必须精确匹配。',
  `LeaseExpiresAt` DATETIME(6) NOT NULL COMMENT '房间所有权以数据库时钟计算的失效时间。',
  `InstanceStatus` TEXT NOT NULL DEFAULT ('active') COMMENT '活动、排空、正常关闭或已隔离失败。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '此实例首次成功分配的数据库时间。',
  CONSTRAINT `CK_RoomInstancesEpoch` CHECK (`OwnerEpoch` > 0),
  CONSTRAINT `CK_RoomInstancesRoom` CHECK (`RoomId` >= 1 AND `RoomId` <= 32767),
  CONSTRAINT `CK_RoomInstancesStatus` CHECK (`InstanceStatus` IN ('active', 'draining', 'closed', 'failed')),
  PRIMARY KEY (`InstanceId`),
  KEY `IX_RoomInstancesExpiry` (`LeaseExpiresAt`),
  `ConditionalUqRoomInstancesLiveRoomRoomId` INT GENERATED ALWAYS AS (CASE WHEN (`InstanceStatus` IN ('active', 'draining')) THEN `RoomId` ELSE NULL END) STORED,
  UNIQUE KEY `UX_RoomInstancesLiveRoom` (`ConditionalUqRoomInstancesLiveRoomRoomId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='独立房间进程的持久身份；关闭后保留以追踪历史对局。';

-- bf-step table ShopBundleItems
CREATE TABLE `ShopBundleItems` (
  `StockId` BIGINT NOT NULL COMMENT '所属 StockSet 销售规格。',
  `Ordinal` INT NOT NULL COMMENT '原版 stockId01 到 stockId10 的有效部件顺序。',
  `SourceStockId` BIGINT NOT NULL COMMENT '原始客户端 stock ID；不依赖单品目录是否挑选该销售规格。',
  `ItemId` BIGINT NOT NULL COMMENT '实际发放的物品模板。',
  `GrantQuantity` BIGINT NOT NULL COMMENT '按模板合并规则恢复的数量。',
  `DurationSeconds` BIGINT COMMENT '部件有效期增量；永久或按数量堆叠时为空。',
  CONSTRAINT `CK_ShopBundleItemsDuration` CHECK (`DurationSeconds` IS NULL OR `DurationSeconds` > 0 AND `GrantQuantity` = 1),
  CONSTRAINT `CK_ShopBundleItemsOrdinal` CHECK (`Ordinal` >= 1 AND `Ordinal` <= 10),
  CONSTRAINT `CK_ShopBundleItemsQuantity` CHECK (`GrantQuantity` > 0),
  CONSTRAINT `CK_ShopBundleItemsSource` CHECK (`SourceStockId` >= 1000000 AND `SourceStockId` <= 1999999),
  PRIMARY KEY (`StockId`, `Ordinal`),
  UNIQUE KEY `UX_ShopBundleItemsTemplate` (`StockId`, `ItemId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='套装有序部件及原始 stock 来源；一个订单原子发放全部部件。';

-- bf-step table ShopBundles
CREATE TABLE `ShopBundles` (
  `StockId` BIGINT NOT NULL COMMENT '原版 stockSetId，也是 C2S809 购买 ID。',
  `DisplayName` TEXT NOT NULL COMMENT '客户端实际加载的本地化套装名称。',
  `DurationType` INT NOT NULL COMMENT '卡片主规格的原版期限编码。',
  `DurationValue` BIGINT NOT NULL COMMENT '卡片展示期限；各部件按独立期限发放。',
  `ShowPrice` BIGINT NOT NULL COMMENT '客户端 StockSet 原始展示价格。',
  CONSTRAINT `CK_ShopBundlesDuration` CHECK (`DurationType` = 1 AND `DurationValue` >= 1 AND `DurationValue` <= 4294967294 OR `DurationType` = 9 AND `DurationValue` = 4294967295),
  CONSTRAINT `CK_ShopBundlesId` CHECK (`StockId` >= 2000000 AND `StockId` <= 2999999),
  CONSTRAINT `CK_ShopBundlesName` CHECK (char_length(`DisplayName`) >= 1 AND char_length(`DisplayName`) <= 128),
  CONSTRAINT `CK_ShopBundlesPrice` CHECK (`ShowPrice` >= 0 AND `ShowPrice` <= 2147483647),
  PRIMARY KEY (`StockId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='原版服装 StockSet；价格及成交币种由 shop_offers 保存，绝不发放虚构的套装模板。';

-- bf-step table ShopOffers
CREATE TABLE `ShopOffers` (
  `StockId` BIGINT NOT NULL COMMENT '客户端购买请求使用的销售规格编号，不得用 item_id 代替。',
  `ItemId` BIGINT NOT NULL COMMENT '该销售规格发放的物品模板编号。',
  `CurrencyCode` VARCHAR(191) NOT NULL COMMENT '扣款币种；bubble_cash 与 treasure_bubble 永远独立。',
  `UnitPrice` BIGINT NOT NULL COMMENT '单件整数成交价，不得为负。',
  `GrantQuantity` BIGINT NOT NULL COMMENT '本规格增加的库存数量；计时型和永久单实例规格固定为 1。',
  `DurationSeconds` BIGINT COMMENT '发放期限秒数；NULL 表示永久。',
  `IsEnabled` BOOLEAN NOT NULL DEFAULT true COMMENT '是否允许新购买；禁用不删除历史订单引用。',
  `ConfigurationVersionId` BIGINT NOT NULL COMMENT '销售规格所属的不可变发布版本。',
  CONSTRAINT `CK_ShopOffersCurrency` CHECK (`CurrencyCode` IN ('lucci', 'bubble_cash', 'treasure_bubble')),
  CONSTRAINT `CK_ShopOffersDurationPositive` CHECK (`DurationSeconds` IS NULL OR `DurationSeconds` > 0),
  CONSTRAINT `CK_ShopOffersEffectShape` CHECK (`DurationSeconds` IS NULL OR `GrantQuantity` = 1),
  CONSTRAINT `CK_ShopOffersPriceNonnegative` CHECK (`UnitPrice` >= 0),
  CONSTRAINT `CK_ShopOffersQuantityPositive` CHECK (`GrantQuantity` > 0),
  CONSTRAINT `CK_ShopOffersStockIdPositive` CHECK (`StockId` > 0),
  PRIMARY KEY (`StockId`),
  KEY `IX_ShopOffersEnabledVersion` (`ConfigurationVersionId`, `StockId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='客户端可购买的 stock 销售规格，固定成交币种、单价、期限和配置版本。';

-- bf-step table SinglePlayAttempts
CREATE TABLE `SinglePlayAttempts` (
  `AttemptId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT 'SinglePlay attempt_id；来自认证配置或独立声明，不授竞技/资产权限',
  `PlayerId` BIGINT NOT NULL COMMENT 'SinglePlay player_id；来自认证配置或独立声明，不授竞技/资产权限',
  `ModeId` INT NOT NULL DEFAULT 15 COMMENT 'SinglePlay mode_id；来自认证配置或独立声明，不授竞技/资产权限',
  `MissionKey` VARCHAR(64) NOT NULL COMMENT 'SinglePlay mission_key；来自认证配置或独立声明，不授竞技/资产权限',
  `Directory` VARCHAR(64) NOT NULL COMMENT 'SinglePlay directory；来自认证配置或独立声明，不授竞技/资产权限',
  `Difficulty` INT NOT NULL COMMENT 'SinglePlay difficulty；来自认证配置或独立声明，不授竞技/资产权限',
  `StageKey` INT NOT NULL COMMENT 'SinglePlay stage_key；来自认证配置或独立声明，不授竞技/资产权限',
  `Authority` TEXT NOT NULL DEFAULT ('client_reported') COMMENT 'SinglePlay authority；来自认证配置或独立声明，不授竞技/资产权限',
  `ProfileHash` TEXT NOT NULL COMMENT 'SinglePlay profile_hash；来自认证配置或独立声明，不授竞技/资产权限',
  `SessionStatus` TEXT NOT NULL DEFAULT ('started') COMMENT 'SinglePlay session_status；来自认证配置或独立声明，不授竞技/资产权限',
  `StartedAt` DATETIME(6) NOT NULL DEFAULT (CURRENT_TIMESTAMP(6)) COMMENT 'SinglePlay started_at；来自认证配置或独立声明，不授竞技/资产权限',
  `FinishedAt` DATETIME(6) COMMENT 'SinglePlay finished_at；来自认证配置或独立声明，不授竞技/资产权限',
  `TerminalReason` TEXT COMMENT 'SinglePlay terminal_reason；来自认证配置或独立声明，不授竞技/资产权限',
  `ElapsedMs` BIGINT COMMENT 'SinglePlay elapsed_ms；来自认证配置或独立声明，不授竞技/资产权限',
  `ExperienceAwarded` BIGINT NOT NULL DEFAULT 0 COMMENT 'SinglePlay experience_awarded；来自认证配置或独立声明，不授竞技/资产权限',
  `LucciAwarded` BIGINT NOT NULL DEFAULT 0 COMMENT 'SinglePlay lucci_awarded；来自认证配置或独立声明，不授竞技/资产权限',
  `RatingDelta` BIGINT NOT NULL DEFAULT 0 COMMENT 'SinglePlay rating_delta；来自认证配置或独立声明，不授竞技/资产权限',
  `ItemAwards` JSON NOT NULL DEFAULT (JSON_ARRAY()) COMMENT 'SinglePlay item_awards；来自认证配置或独立声明，不授竞技/资产权限',
  CONSTRAINT `CK_SingleAttemptMode` CHECK (`ModeId` = 15),
  CONSTRAINT `CK_SingleAttemptReport` CHECK (`SessionStatus` = 'reported_clear' AND `TerminalReason` = 'client_reported_clear' AND `ElapsedMs` IS NOT NULL OR `SessionStatus` = 'reported_failure' AND `TerminalReason` = 'client_reported_failure' AND `ElapsedMs` IS NOT NULL OR (`SessionStatus` IN ('started', 'interrupted')) AND (`TerminalReason` IS NULL OR (`TerminalReason` NOT IN ('client_reported_clear', 'client_reported_failure')))),
  CONSTRAINT `CK_SingleAttemptTerminal` CHECK (`SessionStatus` = 'started' AND `FinishedAt` IS NULL AND `TerminalReason` IS NULL AND `ElapsedMs` IS NULL OR `SessionStatus` <> 'started' AND `FinishedAt` IS NOT NULL AND `TerminalReason` IS NOT NULL AND `FinishedAt` >= `StartedAt`),
  CONSTRAINT `CK_SingleAuthority` CHECK (`Authority` = 'client_reported'),
  CONSTRAINT `CK_SingleElapsed` CHECK (`ElapsedMs` >= 0 AND `ElapsedMs` <= 4294967295),
  CONSTRAINT `CK_SingleProfile` CHECK (REGEXP_LIKE(`ProfileHash`,'^[0-9a-f]{64}$','c')),
  CONSTRAINT `CK_SingleReason` CHECK (REGEXP_LIKE(`TerminalReason`,'^[a-z][a-z0-9_]{0,63}$','c')),
  CONSTRAINT `CK_SingleStatus` CHECK (`SessionStatus` IN ('started', 'reported_clear', 'reported_failure', 'interrupted')),
  CONSTRAINT `CK_SingleZeroItems` CHECK (`ItemAwards` = JSON_ARRAY()),
  CONSTRAINT `CK_SingleZeroLucci` CHECK (`LucciAwarded` = 0),
  CONSTRAINT `CK_SingleZeroRating` CHECK (`RatingDelta` = 0),
  CONSTRAINT `CK_SingleZeroXp` CHECK (`ExperienceAwarded` = 0),
  PRIMARY KEY (`AttemptId`),
  KEY `IX_SingleAttemptHistory` (`PlayerId`, `StageKey`, `StartedAt`),
  `ConditionalUqSingleLivePlayerPlayerId` BIGINT GENERATED ALWAYS AS (CASE WHEN (`SessionStatus` = 'started') THEN `PlayerId` ELSE NULL END) STORED,
  UNIQUE KEY `UX_SingleLivePlayer` (`ConditionalUqSingleLivePlayerPlayerId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='SinglePlay独立原生配置或client_reported账本，零奖励';

-- bf-step table SinglePlayMissions
CREATE TABLE `SinglePlayMissions` (
  `MissionKey` VARCHAR(64) NOT NULL COMMENT 'SinglePlay mission_key；来自认证配置或独立声明，不授竞技/资产权限',
  `Directory` VARCHAR(64) NOT NULL COMMENT 'SinglePlay directory；来自认证配置或独立声明，不授竞技/资产权限',
  `Difficulty` INT NOT NULL COMMENT 'SinglePlay difficulty；来自认证配置或独立声明，不授竞技/资产权限',
  `StageKey` INT NOT NULL COMMENT 'SinglePlay stage_key；来自认证配置或独立声明，不授竞技/资产权限',
  `DurationMs` INT NOT NULL COMMENT 'SinglePlay duration_ms；来自认证配置或独立声明，不授竞技/资产权限',
  CONSTRAINT `CK_SingleMissionDifficulty` CHECK (`Difficulty` >= 1 AND `Difficulty` <= 3),
  CONSTRAINT `CK_SingleMissionDuration` CHECK (`DurationMs` > 0),
  PRIMARY KEY (`MissionKey`),
  UNIQUE KEY `UX_SinglePlayExactMapping` (`MissionKey`, `Directory`, `Difficulty`, `StageKey`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='SinglePlay独立原生配置或client_reported账本，零奖励';

-- bf-step table SoftwareBlocklist
CREATE TABLE `SoftwareBlocklist` (
  `BlocklistId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '黑名单条目标识。',
  `Sha256` VARCHAR(64) NOT NULL COMMENT '被禁软件可执行文件的小写 SHA-256 摘要。',
  `Label` TEXT NOT NULL COMMENT '给管理端看的软件名称或说明。',
  `Category` TEXT NOT NULL COMMENT '软件类别：外挂、宏、注入器、模拟器或其它。',
  `Severity` TEXT NOT NULL DEFAULT ('kick') COMMENT '处置强度：仅告警、踢出对局或短时封禁。',
  `BanSeconds` INT NOT NULL DEFAULT 1800 COMMENT '短时封禁秒数；severity 为 ban 时生效。',
  `Enabled` BOOLEAN NOT NULL DEFAULT true COMMENT '是否生效；关闭后客户端不再匹配该条目。',
  `AddedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '条目加入时间。',
  `AddedBy` TEXT NOT NULL COMMENT '加入该条目的管理端身份。',
  `Notes` TEXT COMMENT '可选备注，说明判定依据或样本来源。',
  CONSTRAINT `CK_SoftwareBlocklistAddedBy` CHECK (char_length(`AddedBy`) >= 1 AND char_length(`AddedBy`) <= 64),
  CONSTRAINT `CK_SoftwareBlocklistBanSeconds` CHECK (`BanSeconds` >= 0 AND `BanSeconds` <= 2592000),
  CONSTRAINT `CK_SoftwareBlocklistCategory` CHECK (`Category` IN ('cheat', 'macro', 'injector', 'emulator', 'other')),
  CONSTRAINT `CK_SoftwareBlocklistLabel` CHECK (char_length(`Label`) >= 1 AND char_length(`Label`) <= 128),
  CONSTRAINT `CK_SoftwareBlocklistNotes` CHECK (`Notes` IS NULL OR char_length(`Notes`) <= 512),
  CONSTRAINT `CK_SoftwareBlocklistSeverity` CHECK (`Severity` IN ('warn', 'kick', 'ban')),
  CONSTRAINT `CK_SoftwareBlocklistSha256` CHECK (REGEXP_LIKE(`Sha256`,'^[0-9a-f]{64}$','c')),
  PRIMARY KEY (`BlocklistId`),
  UNIQUE KEY `UX_SoftwareBlocklistSha256` (`Sha256`),
  KEY `IX_SoftwareBlocklistEnabled` (`Sha256`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='反作弊软件黑名单：按可执行文件摘要匹配，由管理端维护并下发给客户端。';

-- bf-step table SoftwareDetections
CREATE TABLE `SoftwareDetections` (
  `DetectionId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '命中记录标识。',
  `BlocklistId` BIGINT NOT NULL COMMENT '命中的黑名单条目。',
  `Sha256` VARCHAR(64) NOT NULL COMMENT '上报时命中的可执行文件摘要。',
  `AccountId` BIGINT COMMENT '上报账号；未登录时为空。',
  `MachineCode` VARCHAR(69) COMMENT '上报设备摘要；用于设备维度追责。',
  `ProcessName` TEXT NOT NULL COMMENT '命中进程的映像名称。',
  `ProcessPath` TEXT COMMENT '命中进程的完整映像路径。',
  `DetectedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '客户端检出时间。',
  `ActionTaken` TEXT NOT NULL COMMENT '服务端实际采取的动作。',
  `SanctionId` BIGINT COMMENT '由该次命中产生的封禁记录；未封禁时为空。',
  `Details` JSON NOT NULL DEFAULT ('{}') COMMENT '上报的原始上下文，例如进程命令行与父进程信息。',
  CONSTRAINT `CK_SoftwareDetectionsAction` CHECK (`ActionTaken` IN ('reported', 'kicked', 'banned')),
  CONSTRAINT `CK_SoftwareDetectionsProcessName` CHECK (char_length(`ProcessName`) >= 1 AND char_length(`ProcessName`) <= 260),
  CONSTRAINT `CK_SoftwareDetectionsProcessPath` CHECK (`ProcessPath` IS NULL OR char_length(`ProcessPath`) <= 1024),
  CONSTRAINT `CK_SoftwareDetectionsSha256` CHECK (REGEXP_LIKE(`Sha256`,'^[0-9a-f]{64}$','c')),
  CONSTRAINT `CK_SoftwareDetectionsTarget` CHECK (`AccountId` IS NOT NULL OR `MachineCode` IS NOT NULL),
  PRIMARY KEY (`DetectionId`),
  KEY `IX_SoftwareDetectionsAccount` (`AccountId`, `DetectedAt` DESC),
  KEY `IX_SoftwareDetectionsMachine` (`MachineCode`, `DetectedAt` DESC),
  KEY `IX_SoftwareDetectionsSha` (`Sha256`, `DetectedAt` DESC)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='客户端上报的软件命中记录，用于审计与申诉复核。';

-- bf-step table UserHostPrizeClaims
CREATE TABLE `UserHostPrizeClaims` (
  `MatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '债权来源对局。',
  `PlayerId` BIGINT NOT NULL COMMENT '获奖者或原主办者。',
  `ClaimKind` TEXT NOT NULL COMMENT 'payout为净奖金，refund为含手续费全额退款。',
  `Amount` BIGINT NOT NULL COMMENT '本次关闭托管时不可变的金币债权总额。',
  `CreditedAmount` BIGINT NOT NULL DEFAULT 0 COMMENT '已实际入钱包的累计金额，差额必须继续保留。',
  `TicketDue` BOOLEAN NOT NULL DEFAULT false COMMENT '退款时是否还应返还一张永久101164卡。',
  `TicketCredited` BOOLEAN NOT NULL DEFAULT false COMMENT '应退的卡是否已真正写入库存。',
  CONSTRAINT `CK_UserHostPrizeClaimsAmount` CHECK (`Amount` >= 0 AND `Amount` <= 2147483647 AND `CreditedAmount` >= 0 AND `CreditedAmount` <= `Amount`),
  CONSTRAINT `CK_UserHostPrizeClaimsKind` CHECK (`ClaimKind` IN ('payout', 'refund')),
  CONSTRAINT `CK_UserHostPrizeClaimsTicket` CHECK ((NOT `TicketDue` OR `ClaimKind` = 'refund') AND (NOT `TicketCredited` OR `TicketDue`)),
  PRIMARY KEY (`MatchId`, `PlayerId`),
  KEY `IX_UserHostPrizeClaimsPending` (`PlayerId`, `MatchId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='资金及卡债权；客户端余额或库存达到上限时保留差额，之后菜单刷新继续领取，绝不直接截断。';

-- bf-step table UserHostPrizePayments
CREATE TABLE `UserHostPrizePayments` (
  `WalletTransactionId` BIGINT NOT NULL COMMENT '实际写入钱包的唯一流水，含真实前后余额及正增量。',
  `MatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '领取债权所属对局。',
  `PlayerId` BIGINT NOT NULL COMMENT '领取债权的玩家。',
  PRIMARY KEY (`WalletTransactionId`),
  KEY `IX_UserHostPrizePaymentsClaim` (`MatchId`, `PlayerId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='每次实际领取的对局债权与钱包流水关联，可逐笔核对累计入账与剩余债权。';

-- bf-step table UserHostPrizes
CREATE TABLE `UserHostPrizes` (
  `MatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '对局和资金幂等身份；与开局、终局及恢复事务使用同一UUID。',
  `SponsorPlayerId` BIGINT NOT NULL COMMENT '开局冻结的已认证主办者，不要求属于参赛名单。',
  `Amount` BIGINT NOT NULL COMMENT '本局真实扣除的总金币，包含预定手续费。',
  `FeeAmount` BIGINT NOT NULL COMMENT '原生UI可证的整数10%手续费；只有distributed状态实际留存。',
  `PolicyVersion` INT NOT NULL COMMENT '本服分配规则版本，不声称恢复了原服分配规则。',
  `PrizeStatus` TEXT NOT NULL DEFAULT ('funded') COMMENT 'funded托管中，distributed已建立胜者债权，refunded已建立退款债权。',
  `TicketAcquiredAt` DATETIME(6) NOT NULL COMMENT '被消费永久赏金卡原获取时间，退款重新建栈时保留。',
  `FundedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '托管扣款提交时间。',
  `ClosedAt` DATETIME(6) COMMENT '一次性建立奖金或退款债权的时间。',
  CONSTRAINT `CK_UserHostPrizesAmount` CHECK (`Amount` >= 1 AND `Amount` <= 2147483647 AND `FeeAmount` = (`Amount` DIV 10)),
  CONSTRAINT `CK_UserHostPrizesClosed` CHECK ((`PrizeStatus` = 'funded') = (`ClosedAt` IS NULL)),
  CONSTRAINT `CK_UserHostPrizesPolicy` CHECK (`PolicyVersion` = 1),
  CONSTRAINT `CK_UserHostPrizesStatus` CHECK (`PrizeStatus` IN ('funded', 'distributed', 'refunded')),
  PRIMARY KEY (`MatchId`),
  KEY `IX_UserHostPrizesSponsor` (`SponsorPlayerId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='每局唯一奖金托管：开局扣含10%手续费总额及一张101164，净额按本服版本规则分配；中断或平局全退。';

-- bf-step table WalletTransactions
CREATE TABLE `WalletTransactions` (
  `WalletTransactionId` BIGINT NOT NULL AUTO_INCREMENT COMMENT '钱包流水递增主键。',
  `PlayerId` BIGINT NOT NULL COMMENT '资产变更所属玩家。',
  `CurrencyCode` VARCHAR(191) NOT NULL COMMENT '与当前钱包行一致的资产代码。',
  `AmountDelta` BIGINT NOT NULL COMMENT '本次有符号整数变化量；扣款为负，发放为正。',
  `BalanceBefore` BIGINT NOT NULL COMMENT '事务内锁定钱包后观察到的变更前余额。',
  `BalanceAfter` BIGINT NOT NULL COMMENT '同一事务提交的变更后余额。',
  `ReasonCode` VARCHAR(191) NOT NULL COMMENT '购买、奖励、导入期初或补偿等稳定原因代码。',
  `BusinessId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '业务操作稳定 UUID，同一业务和币种对同一玩家只产生一条流水。',
  `OccurredAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '资产变更提交时间。',
  `ImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin COMMENT '迁移期初流水的来源批次；在线业务为 NULL。',
  CONSTRAINT `CK_WalletTransactionsBalanceEquation` CHECK (`BalanceAfter` = (`BalanceBefore` + `AmountDelta`)),
  CONSTRAINT `CK_WalletTransactionsBalancesNonnegative` CHECK (`BalanceBefore` >= 0 AND `BalanceAfter` >= 0),
  CONSTRAINT `CK_WalletTransactionsReasonNonempty` CHECK (char_length(`ReasonCode`) > 0),
  PRIMARY KEY (`WalletTransactionId`),
  UNIQUE KEY `UX_WalletTransactionsBusiness` (`BusinessId`, `CurrencyCode`, `PlayerId`),
  KEY `IX_WalletTransactionsPlayerTime` (`PlayerId`, `OccurredAt`, `WalletTransactionId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='钱包不可变流水；修正通过追加补偿记录完成，不覆盖历史行。';

-- bf-step table WorkshopOperations
CREATE TABLE `WorkshopOperations` (
  `WorkshopOperationId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '服务端稳定业务 UUID，也是新操作幂等身份。',
  `PlayerId` BIGINT NOT NULL COMMENT '执行工坊操作的玩家。',
  `OperationCode` INT NOT NULL COMMENT '已恢复工坊动作代码；未恢复操作不能写成功。',
  `InputSnapshot` JSON NOT NULL COMMENT '事务锁定后实际消耗输入的结构化快照。',
  `RewardSnapshot` JSON NOT NULL COMMENT '同一事务实际产生奖励的结构化快照。',
  `OperationStatus` TEXT NOT NULL COMMENT '状态：pending、succeeded 或 failed。',
  `RulesVersionId` BIGINT NOT NULL COMMENT '操作采用的已发布工坊规则版本。',
  `CreatedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '操作首次受理时间。',
  `CompletedAt` DATETIME(6) COMMENT '成功或明确失败时间；pending 为 NULL。',
  `LegacyActionId` BIGINT COMMENT '旧 SQLite workshop_actions.id；新操作为 NULL。',
  `LegacyImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin COMMENT '旧工坊历史的来源导入批次；导入只归档结果，不重放消耗和奖励。',
  CONSTRAINT `CK_WorkshopOperationsCodePositive` CHECK (`OperationCode` > 0),
  CONSTRAINT `CK_WorkshopOperationsCompletion` CHECK (`OperationStatus` = 'pending' AND `CompletedAt` IS NULL OR (`OperationStatus` IN ('succeeded', 'failed')) AND `CompletedAt` IS NOT NULL),
  CONSTRAINT `CK_WorkshopOperationsInputObject` CHECK (LOWER(JSON_TYPE(`InputSnapshot`)) = 'object'),
  CONSTRAINT `CK_WorkshopOperationsRewardObject` CHECK (LOWER(JSON_TYPE(`RewardSnapshot`)) = 'object'),
  CONSTRAINT `CK_WorkshopOperationsStatus` CHECK (`OperationStatus` IN ('pending', 'succeeded', 'failed')),
  PRIMARY KEY (`WorkshopOperationId`),
  UNIQUE KEY `UX_WorkshopOperationsLegacy` (`LegacyActionId`),
  KEY `IX_WorkshopOperationsPlayerCreated` (`PlayerId`, `CreatedAt`, `WorkshopOperationId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='工坊操作的固定输入、奖励和终态；导入旧记录只保存历史，绝不重放消耗或奖励。';

-- bf-step table LegacyImportRows
CREATE TABLE `LegacyImportRows` (
  `ImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '该原始行所属的旧库导入批次。',
  `SourceTable` VARCHAR(128) NOT NULL COMMENT '该原始行来源的固定旧表名，并与表级元数据组成外键。',
  `SourceOrdinal` BIGINT NOT NULL COMMENT 'JSONL 中从一开始的稳定行号，用于复核顺序和精确定位异常。',
  `RowDigest` VARBINARY(64) NOT NULL COMMENT '不含行结束符的原始 UTF-8 JSON 字节 SHA-256，用于重复暂存一致性检查。',
  `Payload` JSON NOT NULL COMMENT '经独立预检确认字段和标量类型后的原始行 JSON 对象；BLOB 保留十六进制包装。',
  CONSTRAINT `CK_LegacyImportRowsDigestLength` CHECK (octet_length(`RowDigest`) = 32),
  CONSTRAINT `CK_LegacyImportRowsOrdinalPositive` CHECK (`SourceOrdinal` > 0),
  CONSTRAINT `CK_LegacyImportRowsPayloadObject` CHECK (LOWER(JSON_TYPE(`Payload`)) = 'object'),
  PRIMARY KEY (`ImportBatchId`, `SourceTable`, `SourceOrdinal`),
  KEY `IX_LegacyImportRowsDigest` (`ImportBatchId`, `RowDigest`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='逐行保存旧库确定性 JSONL 的规范 JSON 值与原始行摘要；不在暂存阶段猜测或重放业务。';

-- bf-step table LegacyImportTables
CREATE TABLE `LegacyImportTables` (
  `ImportBatchId` CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL COMMENT '关联确定性旧库导入批次；相同快照和导入规则版本重复执行使用同一 UUID。',
  `SourceTable` VARCHAR(128) NOT NULL COMMENT '固定映射中的旧 SQLite 表名，不接受运行时任意标识符。',
  `SourceColumns` JSON NOT NULL COMMENT '旧表字段的稳定有序 JSON 数组，用于防止按错列转换。',
  `SourceRowCount` BIGINT NOT NULL COMMENT '该表通过预检并进入暂存区的原始行数。',
  `SourceDigest` VARBINARY(64) NOT NULL COMMENT '导出清单声明且重新计算一致的 JSONL 文件 SHA-256。',
  `StagedAt` DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) COMMENT '表级元数据首次成功进入 PostgreSQL 的数据库时间。',
  CONSTRAINT `CK_LegacyImportTablesColumnsArray` CHECK (LOWER(JSON_TYPE(`SourceColumns`)) = 'array' AND JSON_LENGTH(`SourceColumns`) > 0),
  CONSTRAINT `CK_LegacyImportTablesDigestLength` CHECK (octet_length(`SourceDigest`) = 32),
  CONSTRAINT `CK_LegacyImportTablesName` CHECK (REGEXP_LIKE(`SourceTable`,'^[a-z][a-z0-9_]*$','c')),
  CONSTRAINT `CK_LegacyImportTablesRowCountNonnegative` CHECK (`SourceRowCount` >= 0),
  PRIMARY KEY (`ImportBatchId`, `SourceTable`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_bin COMMENT='记录已通过独立预检的旧 SQLite 表级字段、行数和文件摘要；规范化转换只能读取已成功批次。';

-- bf-step table AccountCredentials
ALTER TABLE `AccountCredentials`
  ADD CONSTRAINT `FK_AccountCredentialsAccount` FOREIGN KEY (`AccountId`) REFERENCES `Accounts`(`AccountId`);

-- bf-step table AccountDeviceLogins
ALTER TABLE `AccountDeviceLogins`
  ADD CONSTRAINT `FK_AccountDeviceLoginsAccount` FOREIGN KEY (`AccountId`) REFERENCES `Accounts`(`AccountId`),
  ADD CONSTRAINT `FK_AccountDeviceLoginsMachine` FOREIGN KEY (`MachineCode`) REFERENCES `MachineDevices`(`MachineCode`);

-- bf-step table AccountDevices
ALTER TABLE `AccountDevices`
  ADD CONSTRAINT `FK_AccountDevicesAccount` FOREIGN KEY (`AccountId`) REFERENCES `Accounts`(`AccountId`),
  ADD CONSTRAINT `FK_AccountDevicesMachine` FOREIGN KEY (`MachineCode`) REFERENCES `MachineDevices`(`MachineCode`);

-- bf-step table AccountEmails
ALTER TABLE `AccountEmails`
  ADD CONSTRAINT `FK_AccountEmailsAccount` FOREIGN KEY (`AccountId`) REFERENCES `Accounts`(`AccountId`);

-- bf-step table Accounts
ALTER TABLE `Accounts`
  ADD CONSTRAINT `FK_AccountsActiveMachine` FOREIGN KEY (`ActiveMachineCode`) REFERENCES `MachineDevices`(`MachineCode`);

-- bf-step table BubbleDraws
ALTER TABLE `BubbleDraws`
  ADD CONSTRAINT `FK_BubbleDrawsLegacyImport` FOREIGN KEY (`LegacyImportBatchId`) REFERENCES `DataImportBatches`(`ImportBatchId`),
  ADD CONSTRAINT `FK_BubbleDrawsNeedle` FOREIGN KEY (`NeedleItemId`) REFERENCES `ItemTemplates`(`ItemId`),
  ADD CONSTRAINT `FK_BubbleDrawsOperation` FOREIGN KEY (`OperationRequestId`) REFERENCES `OperationRequests`(`OperationRequestId`),
  ADD CONSTRAINT `FK_BubbleDrawsPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`),
  ADD CONSTRAINT `FK_BubbleDrawsRules` FOREIGN KEY (`RulesVersionId`) REFERENCES `ConfigurationVersions`(`ConfigurationVersionId`);

-- bf-step table ChatPenaltyHistory
ALTER TABLE `ChatPenaltyHistory`
  ADD CONSTRAINT `FK_ChatPenaltyHistoryPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table ClanActivityGrants
ALTER TABLE `ClanActivityGrants`
  ADD CONSTRAINT `FK_ClanActivityClan` FOREIGN KEY (`ClanId`) REFERENCES `Clans`(`ClanId`),
  ADD CONSTRAINT `FK_ClanActivityPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table ClanApplications
ALTER TABLE `ClanApplications`
  ADD CONSTRAINT `FK_ClanApplicationsClan` FOREIGN KEY (`ClanId`) REFERENCES `Clans`(`ClanId`),
  ADD CONSTRAINT `FK_ClanApplicationsLegacyImport` FOREIGN KEY (`LegacyImportBatchId`) REFERENCES `DataImportBatches`(`ImportBatchId`),
  ADD CONSTRAINT `FK_ClanApplicationsPlayer` FOREIGN KEY (`ApplicantPlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table ClanAuditEvents
ALTER TABLE `ClanAuditEvents`
  ADD CONSTRAINT `FK_ClanAuditLegacyImport` FOREIGN KEY (`LegacyImportBatchId`) REFERENCES `DataImportBatches`(`ImportBatchId`);

-- bf-step table ClanDisbandRequests
ALTER TABLE `ClanDisbandRequests`
  ADD CONSTRAINT `FK_ClanDisbandClan` FOREIGN KEY (`ClanId`) REFERENCES `Clans`(`ClanId`),
  ADD CONSTRAINT `FK_ClanDisbandLegacyImport` FOREIGN KEY (`LegacyImportBatchId`) REFERENCES `DataImportBatches`(`ImportBatchId`),
  ADD CONSTRAINT `FK_ClanDisbandRequester` FOREIGN KEY (`RequestedByPlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table ClanLeadershipTransfers
ALTER TABLE `ClanLeadershipTransfers`
  ADD CONSTRAINT `FK_ClanTransfersClan` FOREIGN KEY (`ClanId`) REFERENCES `Clans`(`ClanId`),
  ADD CONSTRAINT `FK_ClanTransfersLegacyImport` FOREIGN KEY (`LegacyImportBatchId`) REFERENCES `DataImportBatches`(`ImportBatchId`),
  ADD CONSTRAINT `FK_ClanTransfersSource` FOREIGN KEY (`SourcePlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`),
  ADD CONSTRAINT `FK_ClanTransfersTarget` FOREIGN KEY (`TargetPlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table ClanMatchSides
ALTER TABLE `ClanMatchSides`
  ADD CONSTRAINT `FK_ClanMatchSidesClan` FOREIGN KEY (`ClanId`) REFERENCES `Clans`(`ClanId`),
  ADD CONSTRAINT `FK_ClanMatchSidesMatch` FOREIGN KEY (`MatchId`) REFERENCES `Matches`(`MatchId`);

-- bf-step table ClanMemberships
ALTER TABLE `ClanMemberships`
  ADD CONSTRAINT `FK_ClanMembershipsClan` FOREIGN KEY (`ClanId`) REFERENCES `Clans`(`ClanId`),
  ADD CONSTRAINT `FK_ClanMembershipsLegacyImport` FOREIGN KEY (`LegacyImportBatchId`) REFERENCES `DataImportBatches`(`ImportBatchId`),
  ADD CONSTRAINT `FK_ClanMembershipsPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table ClanMessages
ALTER TABLE `ClanMessages`
  ADD CONSTRAINT `FK_ClanMessagesClan` FOREIGN KEY (`ClanId`) REFERENCES `Clans`(`ClanId`),
  ADD CONSTRAINT `FK_ClanMessagesLegacyImport` FOREIGN KEY (`LegacyImportBatchId`) REFERENCES `DataImportBatches`(`ImportBatchId`);

-- bf-step table ClanPosts
ALTER TABLE `ClanPosts`
  ADD CONSTRAINT `FK_ClanPostsClan` FOREIGN KEY (`ClanId`) REFERENCES `Clans`(`ClanId`),
  ADD CONSTRAINT `FK_ClanPostsLegacyImport` FOREIGN KEY (`LegacyImportBatchId`) REFERENCES `DataImportBatches`(`ImportBatchId`);

-- bf-step table ClanStarterGrants
ALTER TABLE `ClanStarterGrants`
  ADD CONSTRAINT `FK_ClanStarterGrantsImport` FOREIGN KEY (`ImportBatchId`) REFERENCES `DataImportBatches`(`ImportBatchId`),
  ADD CONSTRAINT `FK_ClanStarterGrantsItem` FOREIGN KEY (`ItemId`) REFERENCES `ItemTemplates`(`ItemId`),
  ADD CONSTRAINT `FK_ClanStarterGrantsPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`),
  ADD CONSTRAINT `FK_ClanStarterGrantsTransaction` FOREIGN KEY (`InventoryTransactionId`) REFERENCES `InventoryTransactions`(`InventoryTransactionId`);

-- bf-step table Clans
ALTER TABLE `Clans`
  ADD CONSTRAINT `FK_ClansLeader` FOREIGN KEY (`LeaderPlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`),
  ADD CONSTRAINT `FK_ClansLegacyImport` FOREIGN KEY (`LegacyImportBatchId`) REFERENCES `DataImportBatches`(`ImportBatchId`);

-- bf-step table CoupleDailyGrants
ALTER TABLE `CoupleDailyGrants`
  ADD CONSTRAINT `FK_CoupleDailyGrantsMatch` FOREIGN KEY (`MatchId`) REFERENCES `Matches`(`MatchId`),
  ADD CONSTRAINT `FK_CoupleDailyGrantsPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table CoupleMembers
ALTER TABLE `CoupleMembers`
  ADD CONSTRAINT `FK_CoupleMembersCouple` FOREIGN KEY (`CoupleId`) REFERENCES `Couples`(`CoupleId`),
  ADD CONSTRAINT `FK_CoupleMembersPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table Couples
ALTER TABLE `Couples`
  ADD CONSTRAINT `FK_CouplesRing` FOREIGN KEY (`RingItemId`) REFERENCES `ItemTemplates`(`ItemId`);

-- bf-step table CouponRedemptions
ALTER TABLE `CouponRedemptions`
  ADD CONSTRAINT `FK_CouponRedemptionsAccount` FOREIGN KEY (`AccountId`) REFERENCES `Accounts`(`AccountId`),
  ADD CONSTRAINT `FK_CouponRedemptionsCoupon` FOREIGN KEY (`CouponId`) REFERENCES `Coupons`(`CouponId`),
  ADD CONSTRAINT `FK_CouponRedemptionsPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table Coupons
ALTER TABLE `Coupons`
  ADD CONSTRAINT `FK_CouponsStock` FOREIGN KEY (`StockId`) REFERENCES `ShopOffers`(`StockId`);

-- bf-step table DailyLoginRewards
ALTER TABLE `DailyLoginRewards`
  ADD CONSTRAINT `FK_DailyLoginRewardsPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table EmailBindingSessions
ALTER TABLE `EmailBindingSessions`
  ADD CONSTRAINT `FK_EmailBindingAccount` FOREIGN KEY (`AccountId`) REFERENCES `Accounts`(`AccountId`);

-- bf-step table EmailVerificationChallenges
ALTER TABLE `EmailVerificationChallenges`
  ADD CONSTRAINT `FK_EmailChallengeAccount` FOREIGN KEY (`AccountId`) REFERENCES `Accounts`(`AccountId`);

-- bf-step table ForensicsAttachments
ALTER TABLE `ForensicsAttachments`
  ADD CONSTRAINT `FK_ForensicsAttachmentsReport` FOREIGN KEY (`ReportId`) REFERENCES `ForensicsReports`(`ReportId`);

-- bf-step table ForensicsReports
ALTER TABLE `ForensicsReports`
  ADD CONSTRAINT `FK_ForensicsReportsAccount` FOREIGN KEY (`AccountId`) REFERENCES `Accounts`(`AccountId`),
  ADD CONSTRAINT `FK_ForensicsReportsMachine` FOREIGN KEY (`MachineCode`) REFERENCES `MachineDevices`(`MachineCode`);

-- bf-step table FriendCategories
ALTER TABLE `FriendCategories`
  ADD CONSTRAINT `FK_FriendCategoriesOwnerPlayerId` FOREIGN KEY (`OwnerPlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table FriendCategoryMembers
ALTER TABLE `FriendCategoryMembers`
  ADD CONSTRAINT `FK_FriendCategoryMembersFriendPlayerId` FOREIGN KEY (`FriendPlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`),
  ADD CONSTRAINT `FK_FriendCategoryMembersOwner` FOREIGN KEY (`OwnerPlayerId`, `CategoryId`) REFERENCES `FriendCategories`(`OwnerPlayerId`, `CategoryId`),
  ADD CONSTRAINT `FK_FriendCategoryMembersOwnerPlayerId` FOREIGN KEY (`OwnerPlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table FriendRequests
ALTER TABLE `FriendRequests`
  ADD CONSTRAINT `FK_FriendRequestsLegacyImport` FOREIGN KEY (`LegacyImportBatchId`) REFERENCES `DataImportBatches`(`ImportBatchId`),
  ADD CONSTRAINT `FK_FriendRequestsRecipient` FOREIGN KEY (`RecipientPlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`),
  ADD CONSTRAINT `FK_FriendRequestsSender` FOREIGN KEY (`SenderPlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table Friendships
ALTER TABLE `Friendships`
  ADD CONSTRAINT `FK_FriendshipsFirstPlayer` FOREIGN KEY (`FirstPlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`),
  ADD CONSTRAINT `FK_FriendshipsLegacyImport` FOREIGN KEY (`LegacyImportBatchId`) REFERENCES `DataImportBatches`(`ImportBatchId`),
  ADD CONSTRAINT `FK_FriendshipsSecondPlayer` FOREIGN KEY (`SecondPlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table GroupChatInvitations
ALTER TABLE `GroupChatInvitations`
  ADD CONSTRAINT `FK_GroupChatInvitationsChatId` FOREIGN KEY (`ChatId`) REFERENCES `GroupChats`(`ChatId`),
  ADD CONSTRAINT `FK_GroupChatInvitationsInviterPlayerId` FOREIGN KEY (`InviterPlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`),
  ADD CONSTRAINT `FK_GroupChatInvitationsPlayerId` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table GroupChatMembers
ALTER TABLE `GroupChatMembers`
  ADD CONSTRAINT `FK_GroupChatMembersChatId` FOREIGN KEY (`ChatId`) REFERENCES `GroupChats`(`ChatId`),
  ADD CONSTRAINT `FK_GroupChatMembersPlayerId` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table GroupChatMessages
ALTER TABLE `GroupChatMessages`
  ADD CONSTRAINT `FK_GroupChatMessagesChatId` FOREIGN KEY (`ChatId`) REFERENCES `GroupChats`(`ChatId`),
  ADD CONSTRAINT `FK_GroupChatMessagesSenderPlayerId` FOREIGN KEY (`SenderPlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table HordeTutorSessions
ALTER TABLE `HordeTutorSessions`
  ADD CONSTRAINT `FK_HordeTutorPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table InstantRespawnUses
ALTER TABLE `InstantRespawnUses`
  ADD CONSTRAINT `FK_InstantRespawnUsesParticipant` FOREIGN KEY (`MatchId`, `PlayerId`) REFERENCES `MatchParticipants`(`MatchId`, `PlayerId`);

-- bf-step table InventoryDiscards
ALTER TABLE `InventoryDiscards`
  ADD CONSTRAINT `FK_InventoryDiscardsPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`),
  ADD CONSTRAINT `FK_InventoryDiscardsTemplate` FOREIGN KEY (`NativeItemKey`) REFERENCES `ItemTemplates`(`ItemId`);

-- bf-step table InventoryTransactions
ALTER TABLE `InventoryTransactions`
  ADD CONSTRAINT `FK_InventoryTransactionsImport` FOREIGN KEY (`ImportBatchId`) REFERENCES `DataImportBatches`(`ImportBatchId`),
  ADD CONSTRAINT `FK_InventoryTransactionsInstance` FOREIGN KEY (`ItemInstanceId`, `PlayerId`) REFERENCES `PlayerItems`(`ItemInstanceId`, `PlayerId`),
  ADD CONSTRAINT `FK_InventoryTransactionsPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`),
  ADD CONSTRAINT `FK_InventoryTransactionsTemplate` FOREIGN KEY (`ItemId`) REFERENCES `ItemTemplates`(`ItemId`);

-- bf-step table ItemTemplates
ALTER TABLE `ItemTemplates`
  ADD CONSTRAINT `FK_ItemTemplatesConfiguration` FOREIGN KEY (`ConfigurationVersionId`) REFERENCES `ConfigurationVersions`(`ConfigurationVersionId`);

-- bf-step table LifeAttendanceDays
ALTER TABLE `LifeAttendanceDays`
  ADD CONSTRAINT `FK_LifeAttendancePlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table LifeGuardianEvents
ALTER TABLE `LifeGuardianEvents`
  ADD CONSTRAINT `FK_LifeGuardianPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table LifeItemCursors
ALTER TABLE `LifeItemCursors`
  ADD CONSTRAINT `FK_LifeCursorInstance` FOREIGN KEY (`ItemInstanceId`) REFERENCES `PlayerItems`(`ItemInstanceId`);

-- bf-step table LifeItemSettlements
ALTER TABLE `LifeItemSettlements`
  ADD CONSTRAINT `FK_LifeSettlementPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table LoginTickets
ALTER TABLE `LoginTickets`
  ADD CONSTRAINT `FK_LoginTicketsAccount` FOREIGN KEY (`AccountId`) REFERENCES `Accounts`(`AccountId`),
  ADD CONSTRAINT `FK_LoginTicketsMachine` FOREIGN KEY (`MachineCode`) REFERENCES `MachineDevices`(`MachineCode`);

-- bf-step table MatchCoupleBenefits
ALTER TABLE `MatchCoupleBenefits`
  ADD CONSTRAINT `FK_MatchCoupleBenefitsMatch` FOREIGN KEY (`MatchId`) REFERENCES `Matches`(`MatchId`),
  ADD CONSTRAINT `FK_MatchCoupleBenefitsPartner` FOREIGN KEY (`PartnerPlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`),
  ADD CONSTRAINT `FK_MatchCoupleBenefitsPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`),
  ADD CONSTRAINT `FK_MatchCoupleBenefitsRing` FOREIGN KEY (`RingItemId`) REFERENCES `ItemTemplates`(`ItemId`);

-- bf-step table MatchParticipants
ALTER TABLE `MatchParticipants`
  ADD CONSTRAINT `FK_MatchParticipantsMatch` FOREIGN KEY (`MatchId`) REFERENCES `Matches`(`MatchId`),
  ADD CONSTRAINT `FK_MatchParticipantsPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table MatchRewardGrants
ALTER TABLE `MatchRewardGrants`
  ADD CONSTRAINT `FK_MatchRewardGrantsInventoryTx` FOREIGN KEY (`InventoryTransactionId`) REFERENCES `InventoryTransactions`(`InventoryTransactionId`),
  ADD CONSTRAINT `FK_MatchRewardGrantsItem` FOREIGN KEY (`ItemId`) REFERENCES `ItemTemplates`(`ItemId`),
  ADD CONSTRAINT `FK_MatchRewardGrantsParticipant` FOREIGN KEY (`MatchId`, `PlayerId`) REFERENCES `MatchParticipants`(`MatchId`, `PlayerId`),
  ADD CONSTRAINT `FK_MatchRewardGrantsSettlement` FOREIGN KEY (`SettlementId`) REFERENCES `MatchSettlements`(`SettlementId`),
  ADD CONSTRAINT `FK_MatchRewardGrantsWalletTx` FOREIGN KEY (`WalletTransactionId`) REFERENCES `WalletTransactions`(`WalletTransactionId`);

-- bf-step table MatchSettlements
ALTER TABLE `MatchSettlements`
  ADD CONSTRAINT `FK_MatchSettlementsMatch` FOREIGN KEY (`MatchId`) REFERENCES `Matches`(`MatchId`),
  ADD CONSTRAINT `FK_MatchSettlementsRules` FOREIGN KEY (`RewardRulesVersionId`) REFERENCES `ConfigurationVersions`(`ConfigurationVersionId`);

-- bf-step table Matches
ALTER TABLE `Matches`
  ADD CONSTRAINT `FK_MatchesConfiguration` FOREIGN KEY (`ConfigurationVersionId`) REFERENCES `ConfigurationVersions`(`ConfigurationVersionId`),
  ADD CONSTRAINT `FK_MatchesLegacyImport` FOREIGN KEY (`LegacyImportBatchId`) REFERENCES `DataImportBatches`(`ImportBatchId`),
  ADD CONSTRAINT `FK_MatchesRoomInstance` FOREIGN KEY (`RoomInstanceId`) REFERENCES `RoomInstances`(`InstanceId`);

-- bf-step table MegaphoneMessages
ALTER TABLE `MegaphoneMessages`
  ADD CONSTRAINT `FK_MegaphoneMessagesPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table ModerationAudit
ALTER TABLE `ModerationAudit`
  ADD CONSTRAINT `FK_ModerationAuditAccount` FOREIGN KEY (`AccountId`) REFERENCES `Accounts`(`AccountId`),
  ADD CONSTRAINT `FK_ModerationAuditMachine` FOREIGN KEY (`MachineCode`) REFERENCES `MachineDevices`(`MachineCode`);

-- bf-step table ModerationSanctions
ALTER TABLE `ModerationSanctions`
  ADD CONSTRAINT `FK_ModerationSanctionsAccount` FOREIGN KEY (`AccountId`) REFERENCES `Accounts`(`AccountId`),
  ADD CONSTRAINT `FK_ModerationSanctionsMachine` FOREIGN KEY (`MachineCode`) REFERENCES `MachineDevices`(`MachineCode`);

-- bf-step table MysteryShopSelections
ALTER TABLE `MysteryShopSelections`
  ADD CONSTRAINT `FK_MysterySelectionsConfiguration` FOREIGN KEY (`ConfigurationVersionId`) REFERENCES `ConfigurationVersions`(`ConfigurationVersionId`),
  ADD CONSTRAINT `FK_MysterySelectionsLegacyImport` FOREIGN KEY (`LegacyImportBatchId`) REFERENCES `DataImportBatches`(`ImportBatchId`),
  ADD CONSTRAINT `FK_MysterySelectionsPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table NativeMedalEvents
ALTER TABLE `NativeMedalEvents`
  ADD CONSTRAINT `FK_NativeMedalEventsDefinition` FOREIGN KEY (`MedalKey`) REFERENCES `NativeMedalDefinitions`(`MedalKey`),
  ADD CONSTRAINT `FK_NativeMedalEventsParticipant` FOREIGN KEY (`MatchId`, `PlayerId`) REFERENCES `MatchParticipants`(`MatchId`, `PlayerId`),
  ADD CONSTRAINT `FK_NativeMedalEventsSettlement` FOREIGN KEY (`MatchId`) REFERENCES `MatchSettlements`(`MatchId`);

-- bf-step table NativeTitleClaims
ALTER TABLE `NativeTitleClaims`
  ADD CONSTRAINT `FK_NativeTitleClaimDefinition` FOREIGN KEY (`TitleId`) REFERENCES `NativeTitleDefinitions`(`TitleId`),
  ADD CONSTRAINT `FK_NativeTitleClaimInventory` FOREIGN KEY (`InventoryTransactionId`) REFERENCES `InventoryTransactions`(`InventoryTransactionId`),
  ADD CONSTRAINT `FK_NativeTitleClaimPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table NativeTitleEquipment
ALTER TABLE `NativeTitleEquipment`
  ADD CONSTRAINT `FK_NativeTitleEquipmentDefinition` FOREIGN KEY (`TitleId`) REFERENCES `NativeTitleDefinitions`(`TitleId`),
  ADD CONSTRAINT `FK_NativeTitleEquipmentOwned` FOREIGN KEY (`ItemInstanceId`, `PlayerId`) REFERENCES `PlayerItems`(`ItemInstanceId`, `PlayerId`);

-- bf-step table NativeTitleMedalDebits
ALTER TABLE `NativeTitleMedalDebits`
  ADD CONSTRAINT `FK_NativeTitleDebitClaim` FOREIGN KEY (`ClaimId`) REFERENCES `NativeTitleClaims`(`ClaimId`),
  ADD CONSTRAINT `FK_NativeTitleDebitMedal` FOREIGN KEY (`MedalKey`) REFERENCES `NativeMedalDefinitions`(`MedalKey`),
  ADD CONSTRAINT `FK_NativeTitleDebitPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table NativeTitleRequirements
ALTER TABLE `NativeTitleRequirements`
  ADD CONSTRAINT `FK_NativeTitleRequirementDefinition` FOREIGN KEY (`TitleId`) REFERENCES `NativeTitleDefinitions`(`TitleId`),
  ADD CONSTRAINT `FK_NativeTitleRequirementMedal` FOREIGN KEY (`MedalKey`) REFERENCES `NativeMedalDefinitions`(`MedalKey`);

-- bf-step table NativeTitleSelectionEvents
ALTER TABLE `NativeTitleSelectionEvents`
  ADD CONSTRAINT `FK_NativeTitleSelectionDefinition` FOREIGN KEY (`TitleId`) REFERENCES `NativeTitleDefinitions`(`TitleId`),
  ADD CONSTRAINT `FK_NativeTitleSelectionPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table OperationRequests
ALTER TABLE `OperationRequests`
  ADD CONSTRAINT `FK_OperationRequestsPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table PlayerBlocks
ALTER TABLE `PlayerBlocks`
  ADD CONSTRAINT `FK_PlayerBlocksLegacyImport` FOREIGN KEY (`LegacyImportBatchId`) REFERENCES `DataImportBatches`(`ImportBatchId`),
  ADD CONSTRAINT `FK_PlayerBlocksOwner` FOREIGN KEY (`OwnerPlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`),
  ADD CONSTRAINT `FK_PlayerBlocksTarget` FOREIGN KEY (`BlockedPlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table PlayerChatBlocks
ALTER TABLE `PlayerChatBlocks`
  ADD CONSTRAINT `FK_PlayerChatBlocksOwner` FOREIGN KEY (`OwnerPlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`) ON DELETE CASCADE,
  ADD CONSTRAINT `FK_PlayerChatBlocksTarget` FOREIGN KEY (`BlockedPlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`) ON DELETE CASCADE;

-- bf-step table PlayerEquipment
ALTER TABLE `PlayerEquipment`
  ADD CONSTRAINT `FK_PlayerEquipmentOwnedItem` FOREIGN KEY (`ItemInstanceId`, `PlayerId`) REFERENCES `PlayerItems`(`ItemInstanceId`, `PlayerId`),
  ADD CONSTRAINT `FK_PlayerEquipmentPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table PlayerGiftItems
ALTER TABLE `PlayerGiftItems`
  ADD CONSTRAINT `FK_PlayerGiftItemsGift` FOREIGN KEY (`GiftId`) REFERENCES `PlayerGifts`(`GiftId`),
  ADD CONSTRAINT `FK_PlayerGiftItemsTemplate` FOREIGN KEY (`ItemId`) REFERENCES `ItemTemplates`(`ItemId`);

-- bf-step table PlayerGifts
ALTER TABLE `PlayerGifts`
  ADD CONSTRAINT `FK_PlayerGiftsItem` FOREIGN KEY (`ItemId`) REFERENCES `ItemTemplates`(`ItemId`),
  ADD CONSTRAINT `FK_PlayerGiftsRecipient` FOREIGN KEY (`RecipientId`) REFERENCES `PlayerProfiles`(`PlayerId`),
  ADD CONSTRAINT `FK_PlayerGiftsSender` FOREIGN KEY (`SenderId`) REFERENCES `PlayerProfiles`(`PlayerId`),
  ADD CONSTRAINT `FK_PlayerGiftsStock` FOREIGN KEY (`StockId`) REFERENCES `ShopOffers`(`StockId`);

-- bf-step table PlayerItemAttributes
ALTER TABLE `PlayerItemAttributes`
  ADD CONSTRAINT `FK_ItemAttributeInstance` FOREIGN KEY (`ItemInstanceId`, `PlayerId`) REFERENCES `PlayerItems`(`ItemInstanceId`, `PlayerId`),
  ADD CONSTRAINT `FK_ItemAttributePlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`),
  ADD CONSTRAINT `FK_ItemAttributeTemplate` FOREIGN KEY (`SourceItemId`) REFERENCES `ItemTemplates`(`ItemId`);

-- bf-step table PlayerItems
ALTER TABLE `PlayerItems`
  ADD CONSTRAINT `FK_PlayerItemsLegacyImport` FOREIGN KEY (`LegacyImportBatchId`) REFERENCES `DataImportBatches`(`ImportBatchId`),
  ADD CONSTRAINT `FK_PlayerItemsPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`),
  ADD CONSTRAINT `FK_PlayerItemsTemplate` FOREIGN KEY (`ItemId`) REFERENCES `ItemTemplates`(`ItemId`);

-- bf-step table PlayerKeyBindings
ALTER TABLE `PlayerKeyBindings`
  ADD CONSTRAINT `FK_PlayerKeyBindingsPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table PlayerModeStatistics
ALTER TABLE `PlayerModeStatistics`
  ADD CONSTRAINT `FK_PlayerModeStatisticsPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table PlayerPetGrowth
ALTER TABLE `PlayerPetGrowth`
  ADD CONSTRAINT `FK_PlayerPetGrowthItem` FOREIGN KEY (`ItemInstanceId`, `PlayerId`) REFERENCES `PlayerItems`(`ItemInstanceId`, `PlayerId`),
  ADD CONSTRAINT `FK_PlayerPetGrowthPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`),
  ADD CONSTRAINT `FK_PlayerPetGrowthTemplate` FOREIGN KEY (`PetId`) REFERENCES `ItemTemplates`(`ItemId`);

-- bf-step table PlayerProfiles
ALTER TABLE `PlayerProfiles`
  ADD CONSTRAINT `FK_PlayerProfilesAccount` FOREIGN KEY (`AccountId`) REFERENCES `Accounts`(`AccountId`);

-- bf-step table PlayerProgressions
ALTER TABLE `PlayerProgressions`
  ADD CONSTRAINT `FK_PlayerProgressionsPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`),
  ADD CONSTRAINT `FK_PlayerProgressionsRules` FOREIGN KEY (`RulesVersionId`) REFERENCES `ConfigurationVersions`(`ConfigurationVersionId`);

-- bf-step table PlayerWallets
ALTER TABLE `PlayerWallets`
  ADD CONSTRAINT `FK_PlayerWalletsPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table PrivateMessages
ALTER TABLE `PrivateMessages`
  ADD CONSTRAINT `FK_PrivateMessagesLegacyImport` FOREIGN KEY (`LegacyImportBatchId`) REFERENCES `DataImportBatches`(`ImportBatchId`),
  ADD CONSTRAINT `FK_PrivateMessagesRecipient` FOREIGN KEY (`RecipientPlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`),
  ADD CONSTRAINT `FK_PrivateMessagesSender` FOREIGN KEY (`SenderPlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table PurchaseOrderItems
ALTER TABLE `PurchaseOrderItems`
  ADD CONSTRAINT `FK_PurchaseOrderItemsOrder` FOREIGN KEY (`OrderId`) REFERENCES `PurchaseOrders`(`OrderId`),
  ADD CONSTRAINT `FK_PurchaseOrderItemsStock` FOREIGN KEY (`StockId`) REFERENCES `ShopOffers`(`StockId`),
  ADD CONSTRAINT `FK_PurchaseOrderItemsTemplate` FOREIGN KEY (`ItemId`) REFERENCES `ItemTemplates`(`ItemId`);

-- bf-step table PurchaseOrders
ALTER TABLE `PurchaseOrders`
  ADD CONSTRAINT `FK_PurchaseOrdersLegacyImport` FOREIGN KEY (`LegacyImportBatchId`) REFERENCES `DataImportBatches`(`ImportBatchId`),
  ADD CONSTRAINT `FK_PurchaseOrdersOperation` FOREIGN KEY (`OperationRequestId`) REFERENCES `OperationRequests`(`OperationRequestId`),
  ADD CONSTRAINT `FK_PurchaseOrdersPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table RoomInstances
ALTER TABLE `RoomInstances`
  ADD CONSTRAINT `FK_RoomInstancesNode` FOREIGN KEY (`NodeId`) REFERENCES `DsNodes`(`NodeId`);

-- bf-step table ShopBundleItems
ALTER TABLE `ShopBundleItems`
  ADD CONSTRAINT `FK_ShopBundleItemsBundle` FOREIGN KEY (`StockId`) REFERENCES `ShopBundles`(`StockId`),
  ADD CONSTRAINT `FK_ShopBundleItemsTemplate` FOREIGN KEY (`ItemId`) REFERENCES `ItemTemplates`(`ItemId`);

-- bf-step table ShopBundles
ALTER TABLE `ShopBundles`
  ADD CONSTRAINT `FK_ShopBundlesOffer` FOREIGN KEY (`StockId`) REFERENCES `ShopOffers`(`StockId`);

-- bf-step table ShopOffers
ALTER TABLE `ShopOffers`
  ADD CONSTRAINT `FK_ShopOffersConfiguration` FOREIGN KEY (`ConfigurationVersionId`) REFERENCES `ConfigurationVersions`(`ConfigurationVersionId`),
  ADD CONSTRAINT `FK_ShopOffersItem` FOREIGN KEY (`ItemId`) REFERENCES `ItemTemplates`(`ItemId`);

-- bf-step table SinglePlayAttempts
ALTER TABLE `SinglePlayAttempts`
  ADD CONSTRAINT `FK_SingleAttemptMapping` FOREIGN KEY (`MissionKey`, `Directory`, `Difficulty`, `StageKey`) REFERENCES `SinglePlayMissions`(`MissionKey`, `Directory`, `Difficulty`, `StageKey`),
  ADD CONSTRAINT `FK_SingleAttemptPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table SoftwareDetections
ALTER TABLE `SoftwareDetections`
  ADD CONSTRAINT `FK_SoftwareDetectionsAccount` FOREIGN KEY (`AccountId`) REFERENCES `Accounts`(`AccountId`),
  ADD CONSTRAINT `FK_SoftwareDetectionsBlocklist` FOREIGN KEY (`BlocklistId`) REFERENCES `SoftwareBlocklist`(`BlocklistId`),
  ADD CONSTRAINT `FK_SoftwareDetectionsMachine` FOREIGN KEY (`MachineCode`) REFERENCES `MachineDevices`(`MachineCode`),
  ADD CONSTRAINT `FK_SoftwareDetectionsSanction` FOREIGN KEY (`SanctionId`) REFERENCES `ModerationSanctions`(`SanctionId`);

-- bf-step table UserHostPrizeClaims
ALTER TABLE `UserHostPrizeClaims`
  ADD CONSTRAINT `FK_UserHostPrizeClaimsPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`),
  ADD CONSTRAINT `FK_UserHostPrizeClaimsPrize` FOREIGN KEY (`MatchId`) REFERENCES `UserHostPrizes`(`MatchId`);

-- bf-step table UserHostPrizePayments
ALTER TABLE `UserHostPrizePayments`
  ADD CONSTRAINT `FK_UserHostPrizePaymentsClaim` FOREIGN KEY (`MatchId`, `PlayerId`) REFERENCES `UserHostPrizeClaims`(`MatchId`, `PlayerId`),
  ADD CONSTRAINT `FK_UserHostPrizePaymentsWallet` FOREIGN KEY (`WalletTransactionId`) REFERENCES `WalletTransactions`(`WalletTransactionId`);

-- bf-step table UserHostPrizes
ALTER TABLE `UserHostPrizes`
  ADD CONSTRAINT `FK_UserHostPrizesMatch` FOREIGN KEY (`MatchId`) REFERENCES `Matches`(`MatchId`),
  ADD CONSTRAINT `FK_UserHostPrizesSponsor` FOREIGN KEY (`SponsorPlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`);

-- bf-step table WalletTransactions
ALTER TABLE `WalletTransactions`
  ADD CONSTRAINT `FK_WalletTransactionsImport` FOREIGN KEY (`ImportBatchId`) REFERENCES `DataImportBatches`(`ImportBatchId`),
  ADD CONSTRAINT `FK_WalletTransactionsWallet` FOREIGN KEY (`PlayerId`, `CurrencyCode`) REFERENCES `PlayerWallets`(`PlayerId`, `CurrencyCode`);

-- bf-step table WorkshopOperations
ALTER TABLE `WorkshopOperations`
  ADD CONSTRAINT `FK_WorkshopOperationsLegacyImport` FOREIGN KEY (`LegacyImportBatchId`) REFERENCES `DataImportBatches`(`ImportBatchId`),
  ADD CONSTRAINT `FK_WorkshopOperationsPlayer` FOREIGN KEY (`PlayerId`) REFERENCES `PlayerProfiles`(`PlayerId`),
  ADD CONSTRAINT `FK_WorkshopOperationsRules` FOREIGN KEY (`RulesVersionId`) REFERENCES `ConfigurationVersions`(`ConfigurationVersionId`);

-- bf-step table LegacyImportRows
ALTER TABLE `LegacyImportRows`
  ADD CONSTRAINT `FK_LegacyImportRowsTable` FOREIGN KEY (`ImportBatchId`, `SourceTable`) REFERENCES `LegacyImportTables`(`ImportBatchId`, `SourceTable`);

-- bf-step table LegacyImportTables
ALTER TABLE `LegacyImportTables`
  ADD CONSTRAINT `FK_LegacyImportTablesBatch` FOREIGN KEY (`ImportBatchId`) REFERENCES `DataImportBatches`(`ImportBatchId`);

-- bf-step function AccessoryMask
CREATE FUNCTION `AccessoryMask`(`ItemId` BIGINT) RETURNS BIGINT
DETERMINISTIC NO SQL
RETURN (CASE
 WHEN `ItemId` IN (100076,100077,100159,100181,100370,100371,100372,100373,100374,100375,100376,100817,100818,101100,101229,101790,102107,102431,119998,120532) THEN 2
 WHEN `ItemId` IN (100020,101343,101564,101900,101919,102023,102147,102414,110074,110284) THEN 64
 WHEN `ItemId` IN (100092,100699,100816,100924,100925,100926,100927,100928,100929,100930,100931,100932,100933,100934,100935,101773,102108,102176,190172,190173) THEN 256
 WHEN `ItemId` IN (100032,100033,100043,100044,100068,100078,100110,100156,100164,100221,100288,100289,100340,100341,100342,100343,100356,100357,100474,100535,100599,100642,100653,100654,100668,100727,100728,100784,100815,100868,100869,100883,100977,100978,101022,101057,101063,101081,101151,101152,101162,101163,101180,101206,101383,101394,101454,101558,101607,101618,101869,101920,102115,102172,102205,102213,102351,102358,102368,190050,190142,190146,190147,190149,190150,190152,190155,190174,190175,190179,190200,190202,190244,190280) THEN 1536
 WHEN `ItemId` IN (100250) THEN 2048
 WHEN `ItemId` IN (100086,100143,100157,100253,100344,100345,100346,100347,100348,100349,100350,100423,100576,100577,101868,102148,190027) THEN 8192
 WHEN `ItemId` IN (100063,100085,100145,100241,100506,100593,100594,100595,100651,100678,100990,101092,101208,101909,101956,102068,102152,102280,102305,110280,190022) THEN 16384
 WHEN `ItemId` IN (100024,100141,100225,100242,100256,100257,100486,101066,102149,110282,190025) THEN 32768
 WHEN `ItemId` IN (100050,100057,100062,100140,100222,100310,100319,100377,100378,100379,100380,100596,100620,100697,100700,100845,101067,101093,101447,102069,102150,110140,120140) THEN 65536
 WHEN `ItemId` IN (100046,100111,100182,100578,101065,102438,190024,190049,190070,190078,190157) THEN 131072
 WHEN `ItemId` IN (100142,100144,100224,100258,101064,101161,101207,101342,102177,102274,190023) THEN 16777216
 WHEN `ItemId` IN (100172,100254,100311,100597,100621,100698,100846,101068,101094,101448,102070,102151,110172,120172,190021) THEN 134217728
 WHEN `ItemId` IN (100701,101355,101552,101562,101563,101901,101921,101922,101982,101987,101998,101999,102028,102029,102057,102073,102088,102089,102106,102114,102174,102204,102212,102264,102265,102304,102359,102369,190068,190072,190135,190136,190237) THEN 134283264
 ELSE 0 END);

-- bf-step function AccountBanActive
CREATE FUNCTION `AccountBanActive`(`PAccount` BIGINT, `PMachine` TEXT) RETURNS BOOLEAN
READS SQL DATA
RETURN (COALESCE((SELECT a.`AccountStatus` <> 'active' OR EXISTS (
    SELECT 1 FROM `ModerationSanctions` s WHERE s.`RevokedAt` IS NULL
      AND s.`StartsAt` <= CURRENT_TIMESTAMP(6) AND (s.`ExpiresAt` IS NULL OR s.`ExpiresAt` > CURRENT_TIMESTAMP(6))
      AND ((s.`Kind`='account_ban' AND s.`AccountId`=a.`AccountId`) OR
           (s.`Kind`='device_ban' AND s.`MachineCode`=`PMachine`)))
    FROM `Accounts` a WHERE a.`AccountId`=`PAccount`), true));

-- bf-step function ClanMatchActivityPoints
CREATE FUNCTION `ClanMatchActivityPoints`(`TeamSize` INT, `Outcome` TEXT, `Automatic` BOOLEAN) RETURNS INT
DETERMINISTIC NO SQL
RETURN (CASE WHEN `Outcome`='win' THEN
      CASE `TeamSize` WHEN 2 THEN CASE WHEN `Automatic` THEN 20 ELSE 5 END
                     WHEN 4 THEN CASE WHEN `Automatic` THEN 60 ELSE 15 END ELSE 0 END
    WHEN `Outcome`='draw' THEN
      CASE `TeamSize` WHEN 2 THEN CASE WHEN `Automatic` THEN 10 ELSE 2 END
                     WHEN 4 THEN CASE WHEN `Automatic` THEN 30 ELSE 6 END ELSE 0 END
    ELSE 0 END);

-- bf-step function ClothingCharacterMask
CREATE FUNCTION `ClothingCharacterMask`(`ItemId` BIGINT) RETURNS BIGINT
DETERMINISTIC NO SQL
RETURN (CASE
 WHEN `ItemId` IN (190054) THEN 2
 WHEN `ItemId` IN (190060) THEN 4
 WHEN `ItemId` IN (100011) THEN 6
 WHEN `ItemId` IN (190057) THEN 16
 WHEN `ItemId` IN (190058) THEN 64
 WHEN `ItemId` IN (190059) THEN 128
 WHEN `ItemId` IN (190061) THEN 256
 WHEN `ItemId` IN (100438,100439) THEN 510
 WHEN `ItemId` IN (190062) THEN 1024
 WHEN `ItemId` IN (110072,110073,110101,110103,110129,110130,110152,110153,110154,110155,110232,110233,110234,110235,110236,110270,110271,110272,110273,110274,119999,120101,120103,120129,120130,120152,120153,120154,120155,120156,120157,120158,120159,120160,120232,120233,120234,120235,120236,120237,120238,120239,120240,120241,120335,120521,120522,120523,120524,120525,190040,190041,190042,190043) THEN 1534
 WHEN `ItemId` IN (190064) THEN 2048
 WHEN `ItemId` IN (190065) THEN 16384
 WHEN `ItemId` IN (100685,100686,100687,100688,100689,190056) THEN 524320
 WHEN `ItemId` IN (101911) THEN 33554432
 WHEN `ItemId` IN (190063) THEN 33558528
 WHEN `ItemId` IN (100645,100646,100647,100648,100649,100722,100723,100724,100725,100726,100757,100758,100759,100760,100761,100795,100796,100797,100798,100799,100830,100831,100832,100833,100834,100835,100836,100837,100838,100839,100847,100848,100849,100850,100851,100894,100895,100896,100897,100898,100971,100972,100973,100974,100975,101012,101013,101014,101015,101016,101331,101332,101333,101334,101335,101384,101385,101386,101387,101388,101466,101467,101468,101469,101470,101471,101472,101473,101474,101475,101518,101519,101805,101806,101807,101808,101809,102131,102132,102133,102134,102135,102231,102232,102233,102234,102235,102293,102294,110075,110076,110077,110078,110079,110085,110086,110087,110088,110089) THEN 33559812
 WHEN `ItemId` IN (102370,102371) THEN 134217728
 WHEN `ItemId` IN (190055) THEN 134217736
 WHEN `ItemId` IN (100155,100218,100219,100265,100266,100267,100268,100269,100270,100271,100272,100273,100274,100283,100284,100285,100286,100287,100381,100563,100564,100565,190016,190017) THEN 134219262
 WHEN `ItemId` IN (100003,100004,100006,100007,100035,100037,100047,100048,100049,100072,100073,100074,100096,100101,100103,100105,100106,100108,100109,100114,100115,100116,100121,100122,100124,100125,100126,100128,100129,100130,100133,100134,100137,100138,100139,100147,100148,100149,100150,100151,100152,100153,100154,100160,100161,100162,100163,100227,100228,100229,100230,100231,100232,100233,100234,100235,100236,100245,100246,100247,100248,100249,100260,100261,100262,100263,100264,100275,100276,100277,100320,100321,100322,100323,100324,100325,100326,100327,100328,100329,100330,100331,100332,100333,100334,100401,100402,100403,100404,100405,100426,100427,100428,100429,100430,100444,100445,100446,100447,100448,100475,100476,100477,100478,100479,100501,100502,100503,100504,100505,100515,100516,100517,100518,100519,100520,100521,100530,100531,100532,100533,100534,100571,100572,100573,100574,100575,100589,100590,100591,100592,100612,100613,100614,100615,100616,100617,100618,100619,100987,100988,100989,101255,101256,101257,101258,101259,101260,101261,101262,101263,101264,102214,102215,102216,102217,102218,102219,102220,102221,102222,102223,102224,102225,102226,102227,102228,102241,102242,102243,102244,102245,102246,102247,102248,102249,102250,102251,102252,102253,102254,102255,190030,190031,190032,190033,190034) THEN 134743550
 WHEN `ItemId` IN (100290,100291,100292,100293,100294) THEN 134745598
 WHEN `ItemId` IN (100606,100607,100608,100609,100610) THEN 134759934
 WHEN `ItemId` IN (100658,100659,100660,100661,100662,100717,100718,100719,100720,100721,100785,100786,100787,100788,100789,100790,100791,100792,100793,100794,100825,100826,100827,100828,100829,100852,100853,100854,100855,100856,100884,100885,100886,100887,100888,100889,100890,100891,100892,100893,100979,100980,100981,100982,100983,101052,101053,101054,101055,101056,101321,101322,101323,101324,101325,101389,101390,101391,101392,101393,101437,101438,101439,101440,101441,101456,101457,101458,101459,101460,101461,101462,101463,101464,101465,101514,101515,101810,101811,101812,101813,101814,102058,102059,102060,102061,102062,102063,102064,102065,102066,102067,102136,102137,102138,102139,102140,102236,102237,102238,102239,102240,102298,102299,110080,110081,110082,110083,110084,110090,110091,110092,110093,110094) THEN 134760698
 WHEN `ItemId` IN (100541,100542,100543,100544,100545,100553,100554,100555,100556,100557,100584,100585,100586,100587,100588,100630,100631,100632,100633,100634,100635,100636,100637,100638,100639) THEN 134761982
 WHEN `ItemId` IN (100413,100414,100415,100416,100417,100455,100456,100457,100458,100459) THEN 168318462
 WHEN `ItemId` IN (100117,100118,100119,100120,100173,100174,100175,100176,100177,100203,100204,100205,100206,100207,100208,100209,100210,100211,100212,100213,100214,100215,100216,100217,100240,100295,100296,100297,100298,100299,100335,100336,100337,100338,100339,100358,100359,100360,100361,100362,100363,100364,100365,100366,100367,100418,100419,100420,100421,100422,100433,100434,100435,100436,100437,100449,100450,100451,100452,100453,100510,100511,100512,100513,100514,100566,100567,100568,100569,100570,100579,100580,100581,100582,100583,100611,100663,100664,100665,100666,100667,100690,100691,100692,100693,100694,100702,100703,100704,100705,100706,100729,100730,100731,100732,100733,100734,100735,100736,100737,100738,100742,100743,100744,100745,100746,100747,100748,100749,100750,100751,100752,100753,100754,100755,100756,100762,100763,100764,100765,100766,100767,100768,100769,100770,100771,100772,100773,100800,100801,100802,100803,100804,100805,100806,100807,100808,100809,100810,100811,100812,100813,100814,100918,100919,100920,100921,100922,101002,101003,101004,101005,101006,101017,101018,101019,101020,101021,101022,101023,101024,101025,101026,101027,101029,101030,101031,101032,101033,101070,101071,101072,101073,101074,101076,101077,101078,101079,101080,101087,101088,101089,101090,101091,101095,101096,101097,101098,101099,101101,101102,101103,101104,101105,101106,101107,101108,101109,101110,101156,101157,101158,101159,101160,101170,101171,101172,101173,101174,101175,101176,101177,101178,101179,101181,101182,101183,101184,101185,101186,101187,101188,101189,101190,101191,101192,101193,101194,101195,101196,101197,101198,101199,101200,101201,101202,101203,101204,101205,101214,101215,101216,101217,101218,101219,101220,101221,101222,101223,101235,101236,101237,101238,101239,101240,101241,101242,101243,101244,101245,101246,101247,101248,101249,101250,101251,101252,101253,101254,101265,101266,101267,101268,101269,101271,101272,101273,101274,101275,101276,101277,101278,101279,101280,101281,101282,101283,101284,101285,101286,101287,101288,101289,101290,101306,101307,101308,101309,101310,101311,101312,101313,101314,101315,101345,101346,101347,101348,101349,101350,101351,101352,101353,101354,101357,101358,101359,101360,101361,101366,101367,101368,101369,101370,101397,101398,101399,101400,101401,101407,101408,101409,101410,101411,101412,101413,101414,101415,101416,101417,101418,101419,101420,101421,101449,101450,101451,101452,101453,101480,101481,101482,101483,101484,101485,101486,101487,101488,101489,101490,101491,101500,101501,101502,101503,101504,101510,101511,101512,101513,101522,101523,101524,101525,101526,101527,101528,101529,101530,101531,101532,101542,101543,101544,101545,101546,101547,101548,101549,101550,101551,101553,101554,101555,101556,101557,101570,101571,101572,101573,101574,101585,101586,101587,101588,101589,101590,101591,101592,101593,101594,101595,101596,101597,101598,101599,101600,101601,101602,101603,101604,101608,101609,101610,101611,101612,101619,101620,101621,101622,101623,101624,101625,101626,101627,101628,101629,101630,101631,101632,101633,101635,101636,101640,101641,101642,101643,101644,101645,101646,101647,101648,101649,101688,101689,101690,101691,101692,101693,101694,101695,101696,101697,101698,101699,101700,101701,101702,101703,101704,101705,101706,101707,101708,101709,101721,101722,101723,101724,101725,101727,101728,101729,101730,101731,101732,101733,101734,101735,101736,101737,101738,101739,101740,101741,101742,101743,101751,101752,101753,101754,101755,101756,101757,101758,101759,101763,101764,101768,101769,101775,101776,101777,101778,101779,101795,101796,101797,101798,101799,101800,101801,101802,101803,101804,101828,101829,101830,101831,101832,101833,101834,101835,101836,101837,101838,101839,101840,101841,101842,101843,101844,101845,101846,101847,101848,101849,101850,101851,101852,101853,101871,101872,101873,101874,101875,101876,101877,101878,101879,101880,101881,101882,101883,101884,101885,101886,101887,101888,101889,101890,101891,101892,101893,101895,101896,101897,101898,101899,101913,101914,101915,101916,101917,101925,101926,101927,101928,101929,101930,101931,101932,101933,101934,101935,101936,101937,101938,101939,101940,101941,101942,101944,101945,101950,101951,101952,101953,101954,101964,101965,101966,101967,101968,101969,101970,101971,101972,101973,101974,101975,101976,101977,101978,101983,101984,101985,101986,102000,102001,102002,102003,102004,102006,102007,102008,102009,102010,102012,102013,102014,102015,102018,102019,102020,102021,102022,102024,102025,102026,102027,102035,102036,102037,102038,102039,102052,102053,102054,102055,102056,102071,102072,102074,102075,102076,102077,102078,102079,102080,102081,102082,102083,102084,102085,102093,102094,102095,102096,102097,102098,102101,102102,102103,102104,102105,102109,102110,102111,102112,102113,102117,102118,102119,102120,102121,102122,102123,102124,102125,102126,102127,102128,102129,102130,102143,102144,102145,102146,102167,102168,102178,102179,102180,102181,102182,102183,102184,102185,102186,102187,102188,102189,102190,102191,102192,102193,102194,102195,102196,102199,102200,102201,102202,102203,102207,102208,102209,102210,102211,102256,102257,102272,102273,102275,102276,102277,102278,102279,102281,102282,102283,102284,102285,102307,102308,102309,102310,102312,102313,102314,102315,102316,102317,102318,102320,102321,102322,102323,102324,102329,102330,102331,102332,102333,102339,102340,102341,102342,102343,102344,102345,102346,102347,102348,102349,102350,102352,102353,102356,102357,102361,102362,102363,102364,102365,102366,102367,102442,102443,110096,110097,110098,110099,110100,190114,190115,190116,190117,190118,190119,190120,190121,190122,190123,190124,190125) THEN 168320510
 WHEN `ItemId` IN (100001,100002,100020,100024,100032,100033,100043,100044,100046,100050,100057,100062,100063,100064,100065,100066,100067,100068,100076,100077,100078,100079,100080,100081,100082,100083,100085,100086,100090,100091,100092,100104,100110,100111,100112,100140,100141,100142,100143,100144,100145,100156,100157,100159,100164,100171,100172,100178,100179,100180,100181,100182,100183,100184,100185,100186,100187,100188,100189,100190,100191,100192,100193,100194,100195,100196,100197,100198,100199,100200,100201,100202,100221,100222,100224,100225,100226,100241,100242,100250,100251,100252,100253,100254,100256,100257,100258,100288,100289,100310,100311,100319,100340,100341,100342,100343,100344,100345,100346,100347,100348,100349,100350,100356,100357,100370,100371,100372,100373,100374,100375,100376,100377,100378,100379,100380,100382,100383,100384,100385,100386,100412,100423,100425,100431,100432,100443,100460,100461,100462,100463,100464,100465,100466,100467,100468,100469,100470,100471,100473,100474,100480,100481,100482,100483,100484,100485,100486,100499,100500,100506,100507,100508,100509,100535,100546,100547,100548,100576,100577,100578,100593,100594,100595,100596,100597,100598,100599,100604,100605,100620,100621,100622,100623,100624,100625,100626,100627,100628,100629,100640,100641,100642,100643,100644,100650,100651,100652,100653,100654,100657,100668,100678,100679,100680,100681,100682,100683,100684,100697,100698,100699,100700,100701,100707,100708,100709,100710,100711,100727,100728,100784,100815,100816,100817,100818,100819,100840,100841,100842,100843,100844,100845,100846,100857,100858,100859,100860,100861,100868,100869,100870,100871,100872,100873,100874,100883,100899,100900,100901,100902,100903,100904,100924,100925,100926,100927,100928,100929,100930,100931,100932,100933,100934,100935,100941,100942,100943,100944,100945,100946,100947,100948,100949,100950,100956,100957,100958,100959,100960,100966,100967,100968,100969,100970,100976,100977,100978,100984,100985,100986,100990,100991,100992,100993,100994,100995,100996,100997,100998,100999,101000,101007,101008,101009,101010,101011,101028,101034,101035,101036,101037,101038,101039,101040,101046,101050,101057,101062,101063,101064,101065,101066,101067,101068,101069,101081,101092,101093,101094,101100,101151,101152,101161,101162,101163,101164,101180,101206,101207,101208,101229,101291,101342,101343,101344,101355,101383,101394,101395,101396,101447,101448,101454,101455,101552,101558,101560,101561,101562,101563,101564,101605,101606,101607,101618,101744,101745,101773,101786,101787,101790,101825,101856,101862,101868,101869,101870,101900,101901,101902,101903,101904,101905,101906,101907,101908,101909,101918,101919,101920,101921,101922,101923,101949,101956,101962,101963,101979,101980,101981,101982,101987,101988,101998,101999,102023,102028,102029,102057,102068,102069,102070,102073,102086,102088,102089,102106,102107,102108,102114,102115,102116,102141,102142,102147,102148,102149,102150,102151,102152,102153,102154,102160,102161,102162,102163,102164,102165,102166,102172,102173,102174,102175,102176,102177,102204,102205,102212,102213,102261,102264,102265,102274,102280,102303,102304,102305,102351,102358,102359,102368,102369,102414,102431,102438,102611,102640,110074,110140,110172,110280,110282,110284,119998,120140,120172,120520,120532,180001,180002,180003,180004,180005,180006,190021,190022,190023,190024,190025,190027,190047,190048,190049,190050,190051,190068,190069,190070,190071,190072,190074,190075,190076,190078,190081,190082,190083,190085,190086,190087,190088,190102,190103,190105,190106,190107,190108,190109,190110,190111,190127,190128,190132,190133,190135,190136,190137,190138,190139,190141,190142,190144,190145,190146,190147,190149,190150,190151,190152,190154,190155,190156,190157,190158,190159,190160,190164,190165,190166,190167,190168,190169,190170,190172,190173,190174,190175,190176,190178,190179,190185,190193,190194,190195,190197,190198,190200,190202,190205,190206,190207,190208,190209,190210,190211,190212,190213,190214,190219,190222,190228,190231,190237,190243,190244,190247,190248,190249,190250,190251,190252,190254,190255,190256,190260,190261,190262,190264,190266,190267,190268,190269,190270,190273,190274,190275,190276,190277,190280,190285,190286) THEN 4294967295
 ELSE NULL END);

-- bf-step function ClothingMask
CREATE FUNCTION `ClothingMask`(`ItemId` BIGINT) RETURNS BIGINT
DETERMINISTIC NO SQL
RETURN (CASE
 WHEN `ItemId` IN (100007,100035,100037,100047,100072,100074,100096,100101,100105,100147,100152,100160,100173,100183,100184,100185,100186,100203,100208,100213,100218,100227,100232,100240,100245,100260,100265,100270,100283,100290,100295,100320,100325,100330,100335,100358,100363,100401,100413,100418,100426,100433,100444,100449,100455,100475,100501,100510,100515,100520,100530,100541,100553,100563,100564,100566,100571,100579,100584,100606,100611,100612,100616,100630,100635,100645,100658,100663,100680,100685,100690,100702,100707,100717,100722,100729,100734,100742,100747,100752,100757,100762,100764,100769,100785,100790,100795,100800,100805,100810,100825,100830,100835,100840,100847,100852,100857,100870,100884,100889,100894,100900,100918,100941,100946,100956,100966,100971,100979,100987,100991,100996,101002,101007,101012,101017,101023,101029,101036,101052,101070,101076,101087,101095,101101,101106,101156,101170,101175,101181,101186,101191,101196,101201,101214,101219,101235,101240,101245,101250,101255,101260,101265,101271,101276,101281,101286,101306,101311,101321,101331,101345,101350,101357,101366,101384,101389,101397,101407,101412,101417,101437,101449,101456,101461,101466,101471,101480,101485,101490,101500,101510,101512,101514,101518,101522,101527,101542,101547,101553,101570,101585,101590,101595,101600,101608,101619,101624,101629,101635,101640,101645,101688,101693,101698,101700,101705,101721,101727,101729,101734,101739,101751,101753,101758,101763,101768,101775,101795,101800,101805,101810,101828,101830,101832,101837,101842,101847,101852,101871,101873,101875,101880,101885,101887,101889,101913,101925,101930,101935,101937,101939,101941,101944,101950,101964,101969,101974,101983,101985,102000,102006,102012,102014,102018,102024,102026,102035,102052,102058,102063,102071,102074,102079,102084,102093,102095,102097,102101,102109,102117,102122,102124,102129,102131,102136,102143,102145,102178,102183,102188,102190,102192,102199,102207,102214,102219,102224,102231,102236,102241,102246,102251,102256,102272,102275,102281,102293,102298,102307,102309,102312,102314,102320,102329,102339,102341,102346,102352,102356,102361,102366,102370,102442,110072,110075,110080,110085,110090,110096,110101,110152,110232,110270,119999,120101,120152,120156,120232,120237,120521,190016,190030,190040,190114,190116,190121) THEN 2
 WHEN `ItemId` IN (100106,100899) THEN 6
 WHEN `ItemId` IN (100108,100119,100150,100154,100176,100195,100196,100197,100198,100206,100211,100216,100219,100230,100235,100248,100263,100268,100273,100286,100293,100298,100323,100328,100333,100338,100361,100366,100404,100416,100421,100429,100436,100447,100452,100458,100478,100504,100513,100518,100533,100544,100556,100569,100574,100582,100587,100591,100609,100633,100638,100648,100661,100666,100683,100688,100693,100705,100710,100720,100725,100732,100737,100745,100750,100755,100760,100767,100772,100788,100793,100798,100803,100808,100813,100828,100833,100838,100843,100850,100855,100860,100873,100887,100892,100897,100903,100921,100944,100949,100959,100969,100974,100982,100994,100999,101005,101010,101015,101020,101026,101032,101039,101055,101073,101079,101090,101098,101104,101109,101159,101173,101178,101184,101189,101194,101199,101204,101217,101222,101238,101243,101248,101253,101258,101263,101268,101274,101279,101284,101289,101309,101314,101324,101334,101348,101353,101360,101369,101387,101392,101400,101410,101415,101420,101440,101452,101459,101464,101469,101474,101483,101488,101503,101525,101530,101545,101550,101556,101573,101588,101593,101598,101603,101611,101622,101627,101632,101643,101648,101691,101696,101703,101708,101724,101732,101737,101742,101756,101778,101798,101803,101808,101813,101835,101840,101845,101850,101878,101883,101892,101916,101928,101933,101953,101967,101972,101977,102003,102009,102021,102038,102055,102061,102066,102077,102082,102104,102112,102120,102127,102134,102139,102181,102186,102195,102202,102210,102217,102222,102227,102234,102239,102244,102249,102254,102278,102284,102317,102323,102332,102344,102349,102364,110078,110083,110088,110093,110099,110154,110235,110273,120154,120159,120235,120240,120335,120524,190033,190042,190119,190124) THEN 96
 WHEN `ItemId` IN (100048,100988) THEN 128
 WHEN `ItemId` IN (100004,100006,100114,100117,100121,100125,100129,100133,100137,100148,100161,100174,100187,100188,100189,100190,100204,100209,100214,100228,100233,100246,100261,100266,100284,100291,100296,100321,100326,100331,100336,100359,100364,100381,100402,100414,100419,100427,100434,100438,100445,100450,100456,100476,100502,100511,100516,100531,100542,100554,100565,100567,100572,100580,100585,100589,100607,100613,100617,100631,100636,100646,100659,100664,100681,100686,100691,100703,100708,100718,100723,100730,100735,100743,100748,100753,100758,100765,100770,100786,100791,100796,100801,100806,100811,100826,100831,100836,100841,100848,100853,100858,100871,100885,100890,100895,100901,100919,100942,100947,100957,100967,100972,100980,100992,100997,101003,101008,101013,101018,101024,101030,101037,101053,101071,101077,101088,101096,101102,101107,101157,101171,101176,101182,101187,101192,101197,101202,101215,101220,101236,101241,101246,101251,101256,101261,101266,101272,101277,101282,101287,101307,101312,101322,101332,101346,101351,101358,101367,101385,101390,101398,101408,101413,101418,101438,101450,101457,101462,101467,101472,101481,101486,101501,101523,101528,101543,101548,101554,101571,101586,101591,101596,101601,101609,101620,101625,101630,101641,101646,101689,101694,101701,101706,101722,101730,101735,101740,101754,101776,101796,101801,101806,101811,101833,101838,101843,101848,101876,101881,101890,101914,101926,101931,101951,101965,101970,101975,102001,102007,102019,102036,102053,102059,102064,102075,102080,102110,102118,102125,102132,102137,102179,102184,102193,102200,102208,102215,102220,102225,102232,102237,102242,102247,102252,102276,102282,102315,102321,102330,102342,102347,102362,110076,110081,110086,110091,110097,110129,110233,120129,120157,120233,120238,120522,190031,190117,190122) THEN 152
 WHEN `ItemId` IN (100115,100118,100122,100126,100130,100134,100138,100149,100162,100175,100191,100192,100193,100194,100205,100210,100215,100229,100234,100247,100262,100267,100272,100275,100276,100277,100285,100292,100297,100322,100327,100332,100337,100360,100365,100403,100415,100420,100428,100435,100439,100446,100451,100457,100477,100503,100512,100517,100532,100543,100555,100568,100573,100581,100586,100590,100608,100614,100618,100632,100637,100647,100660,100665,100682,100687,100692,100704,100709,100719,100724,100731,100736,100744,100749,100754,100759,100766,100771,100787,100792,100797,100802,100807,100812,100827,100832,100837,100842,100849,100854,100859,100872,100886,100891,100896,100902,100920,100943,100948,100958,100968,100973,100981,100993,100998,101004,101009,101014,101019,101025,101031,101038,101054,101072,101078,101089,101097,101103,101108,101158,101172,101177,101183,101188,101193,101198,101203,101216,101221,101237,101242,101247,101252,101257,101262,101267,101273,101278,101283,101288,101308,101313,101323,101333,101347,101352,101359,101368,101386,101391,101399,101409,101414,101419,101439,101451,101458,101463,101468,101473,101482,101487,101502,101524,101529,101544,101549,101555,101572,101587,101592,101597,101602,101610,101621,101626,101631,101642,101647,101690,101695,101702,101707,101723,101731,101736,101741,101755,101777,101797,101802,101807,101812,101834,101839,101844,101849,101877,101882,101891,101915,101927,101932,101952,101966,101971,101976,102002,102008,102020,102037,102054,102060,102065,102076,102081,102103,102111,102119,102126,102133,102138,102180,102185,102194,102201,102209,102216,102221,102226,102233,102238,102243,102248,102253,102277,102283,102316,102322,102331,102343,102348,102363,110077,110082,110087,110092,110098,110130,110234,110272,120130,120158,120234,120239,120523,190032,190118,190123) THEN 768
 WHEN `ItemId` IN (100153,110153,120153,190041) THEN 920
 WHEN `ItemId` IN (100003,100049,100103,100109,100116,100120,100124,100128,100139,100151,100155,100163,100177,100199,100200,100201,100202,100207,100212,100217,100231,100236,100249,100264,100269,100274,100287,100294,100299,100324,100329,100334,100339,100362,100367,100405,100417,100422,100430,100437,100448,100453,100459,100479,100505,100514,100519,100534,100545,100557,100570,100575,100583,100588,100592,100610,100615,100619,100634,100639,100649,100662,100667,100684,100689,100694,100706,100711,100721,100726,100733,100738,100746,100751,100756,100761,100768,100773,100789,100794,100799,100804,100809,100814,100829,100834,100839,100844,100851,100856,100861,100874,100888,100893,100898,100904,100922,100945,100950,100960,100970,100975,100983,100989,100995,101000,101006,101011,101016,101021,101027,101033,101040,101056,101074,101080,101091,101099,101105,101110,101160,101174,101179,101185,101190,101195,101200,101205,101218,101223,101239,101244,101249,101254,101259,101264,101269,101275,101280,101285,101290,101310,101315,101325,101335,101349,101354,101361,101370,101388,101393,101401,101411,101416,101421,101441,101453,101460,101465,101470,101475,101484,101489,101504,101526,101531,101546,101551,101557,101574,101589,101594,101599,101604,101612,101623,101628,101633,101644,101649,101692,101697,101704,101709,101725,101733,101738,101743,101757,101779,101799,101804,101809,101814,101836,101841,101846,101851,101879,101884,101893,101917,101929,101934,101954,101968,101973,101978,102004,102010,102022,102039,102056,102062,102067,102078,102083,102105,102113,102121,102128,102135,102140,102182,102187,102196,102203,102211,102218,102223,102228,102235,102240,102245,102250,102255,102279,102285,102318,102324,102333,102345,102350,102365,110079,110084,110089,110094,110100,110103,110155,110236,110274,120103,120155,120160,120236,120241,120525,190034,190043,190120,190125) THEN 1024
 WHEN `ItemId` IN (100073,100271,100521,100763,101491,101511,101513,101515,101519,101636,101699,101728,101752,101759,101764,101769,101829,101831,101853,101872,101874,101886,101888,101936,101938,101940,101942,101945,101984,101986,102013,102015,102025,102027,102072,102085,102094,102096,102098,102102,102123,102130,102144,102146,102189,102191,102257,102273,102294,102299,102308,102310,102313,102340,102353,102357,102367,102371,102443,110073,110271,190017,190115) THEN 2040
 WHEN `ItemId` IN (100011,101532,101911,190054,190055,190056,190057,190058,190059,190060,190061,190062,190063,190064,190065,190076) THEN 2042
 ELSE 0 END);

-- bf-step function EquippedCharacter
CREATE FUNCTION `EquippedCharacter`(`Player` BIGINT, `AtTime` DATETIME(6)) RETURNS BIGINT
READS SQL DATA
RETURN (coalesce((SELECT i.`ItemId` FROM `PlayerEquipment` e
 JOIN `PlayerItems` i USING(`PlayerId`,`ItemInstanceId`)
 WHERE e.`PlayerId`=`Player` AND e.`LoadoutId`=0 AND e.`SlotCode`='character'
 AND i.`Quantity`>0 AND (i.`ExpiresAt` IS NULL OR i.`ExpiresAt`>`AtTime`)),
 (SELECT 300000+`InitialFighterId` FROM `PlayerProfiles` WHERE `PlayerId`=`Player`)));

-- bf-step function IsAdvancedClothingItem
CREATE FUNCTION `IsAdvancedClothingItem`(`Candidate` BIGINT) RETURNS BOOLEAN
DETERMINISTIC NO SQL
RETURN (`Candidate` IN (
    100260,100261,100262,100263,100264,100358,100359,100360,100361,100362,100363,100364,
    100365,100366,100367,100413,100414,100415,100416,100417,100418,100419,100420,100421,
    100422,100426,100427,100428,100429,100430,100433,100434,100435,100436,100437,100444,
    100445,100446,100447,100448,100449,100450,100451,100452,100453,100455,100456,100457,
    100458,100459,100501,100502,100503,100504,100505,100510,100511,100512,100513,100514,
    100515,100516,100517,100518,100519,100530,100531,100532,100533,100534,100541,100542,
    100543,100544,100545,100553,100554,100555,100556,100557,100566,100567,100568,100569,
    100570,100571,100572,100573,100574,100575,100579,100580,100581,100582,100583,100584,
    100585,100586,100587,100588,100606,100607,100608,100609,100610,100612,100613,100614,
    100615,100616,100617,100618,100619,100630,100631,100632,100633,100634,100635,100636,
    100637,100638,100639,100645,100646,100647,100648,100649,100658,100659,100660,100661,
    100662,100663,100664,100665,100666,100667,100680,100681,100682,100683,100684,100685,
    100686,100687,100688,100689,100690,100691,100692,100693,100694,100702,100703,100704,
    100705,100706,100707,100708,100709,100710,100711,100717,100718,100719,100720,100721,
    100722,100723,100724,100725,100726,100729,100730,100731,100732,100733,100734,100735,
    100736,100737,100738,100742,100743,100744,100745,100746,100747,100748,100749,100750,
    100751,100752,100753,100754,100755,100756,100757,100758,100759,100760,100761,100762,
    100763,100764,100765,100766,100767,100768,100769,100770,100771,100772,100773,100785,
    100786,100787,100788,100789,100790,100791,100792,100793,100794,100795,100796,100797,
    100798,100799,100800,100801,100802,100803,100804,100805,100806,100807,100808,100809,
    100810,100811,100812,100813,100814,100825,100826,100827,100828,100829,100830,100831,
    100832,100833,100834,100835,100836,100837,100838,100839,100840,100841,100842,100843,
    100844,100847,100848,100849,100850,100851,100852,100853,100854,100855,100856,100857,
    100858,100859,100860,100861,100884,100885,100886,100887,100888,100889,100890,100891,
    100892,100893,100894,100895,100896,100897,100898,100900,100901,100902,100903,100904,
    100918,100919,100920,100921,100922,100941,100942,100943,100944,100945,100946,100947,
    100948,100949,100950,100956,100957,100958,100959,100960,100966,100967,100968,100969,
    100970,100971,100972,100973,100974,100975,100979,100980,100981,100982,100983,100987,
    100988,100989,100991,100992,100993,100994,100995,100996,100997,100998,100999,101000,
    101002,101003,101004,101005,101006,101007,101008,101009,101010,101011,101012,101013,
    101014,101015,101016,101017,101018,101019,101020,101021,101023,101024,101025,101026,
    101027,101029,101030,101031,101032,101033,101036,101037,101038,101039,101040,101052,
    101053,101054,101055,101056,101070,101071,101072,101073,101074,101076,101077,101078,
    101079,101080,101087,101088,101089,101090,101091,101095,101096,101097,101098,101099,
    101101,101102,101103,101104,101105,101106,101107,101108,101109,101110,101156,101157,
    101158,101159,101160,101170,101171,101172,101173,101174,101175,101176,101177,101178,
    101179,101186,101187,101188,101189,101190,101191,101192,101193,101194,101195,101196,
    101197,101198,101199,101200,101201,101202,101203,101204,101205,101214,101215,101216,
    101217,101218,101235,101236,101237,101238,101239,101321,101322,101323,101324,101325,
    101331,101332,101333,101334,101335,101345,101346,101347,101348,101349,101357,101358,
    101359,101360,101361,101384,101385,101386,101387,101388,101389,101390,101391,101392,
    101393,101407,101408,101409,101410,101411,101417,101418,101419,101420,101421,101437,
    101438,101439,101440,101441,101456,101457,101458,101459,101460,101466,101467,101468,
    101469,101470,101480,101481,101482,101483,101484,101490,101491,101510,101511,101514,
    101515,101518,101519,101522,101523,101524,101525,101526,101532,101542,101543,101544,
    101545,101546,101570,101571,101572,101573,101574,101585,101586,101587,101588,101589,
    101608,101609,101610,101611,101612,101619,101620,101621,101622,101623,101635,101636,
    101688,101689,101690,101691,101692,101698,101699,101705,101706,101707,101708,101709,
    101727,101728,101729,101730,101731,101732,101733,101734,101735,101736,101737,101738,
    101751,101752,101775,101776,101777,101778,101779,101795,101796,101797,101798,101799,
    101805,101806,101807,101808,101809,101810,101811,101812,101813,101814,101828,101829,
    101832,101833,101834,101835,101836,101842,101843,101844,101845,101846,101871,101872,
    101875,101876,101877,101878,101879,101885,101886,101889,101890,101891,101892,101893,
    101911,101925,101926,101927,101928,101929,101930,101931,101932,101933,101934,101935,
    101936,101939,101940,101950,101951,101952,101953,101954,101964,101965,101966,101967,
    101968,101983,101984,102000,102001,102002,102003,102004,102012,102013,102018,102019,
    102020,102021,102022,102024,102025,102035,102036,102037,102038,102039,102052,102053,
    102054,102055,102056,102058,102059,102060,102061,102062,102074,102075,102076,102077,
    102078,102084,102085,102093,102094,102101,102102,102109,102110,102111,102112,102113,
    102117,102118,102119,102120,102121,102124,102125,102126,102127,102128,102129,102130,
    102143,102144,102178,102179,102180,102181,102182,102188,102189,102192,102193,102194,
    102195,102196,102199,102200,102201,102202,102203,102207,102208,102209,102210,102211,
    102272,102273,102275,102276,102277,102278,102279,102307,102308,102309,102310,102339,
    102340,102341,102342,102343,102344,102345,102346,102347,102348,102349,102350,102356,
    102357,102370,102371,102442,102443,110075,110076,110077,110078,110079,110080,110081,
    110082,110083,110084,110085,110086,110087,110088,110089,110090,110091,110092,110093,
    110094,110096,110097,110098,110099,110100,190054,190055,190056,190057,190058,190059,
    190060,190061,190062,190063,190064,190065,190076,190114,190115
  ));

-- bf-step function IsLifeItem
CREATE FUNCTION `IsLifeItem`(`Item` BIGINT) RETURNS BOOLEAN
DETERMINISTIC NO SQL
RETURN (`Item` IN (
  101447,101448,101449,101450,101451,101452,101453,101454,101455,101700,
  101701,101702,101703,101704,101786,101856,101862,101920,101922,101956,
  101987,101999,102029,102068,102069,102070,102073,102089,102115,102147,
  102265,102314,102315,102316,102317,102318,102369,190149,190150,190155,
  190157,190175,190202,190244,201337,201350,201359,201362,201923,201924,
  202339,202386,202394,202400,202924,202938,202939,202956,203381,203390,
  203395,203925,203926,203934,203935,203949,204131,204375,204382,204388,
  204924,204933,204934,204944,207925,207928,207941,207946,207952,207975
 ));

-- bf-step function SoftwareBlocklistAction
CREATE FUNCTION `SoftwareBlocklistAction`(`PSha256` TEXT) RETURNS TEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin
READS SQL DATA
RETURN (COALESCE((SELECT b.`Severity` FROM `SoftwareBlocklist` b
    WHERE b.`Sha256` = lower(`PSha256`) AND b.`Enabled`), 'allow'));

-- bf-step function SoftwareBlocklistBanSeconds
CREATE FUNCTION `SoftwareBlocklistBanSeconds`(`PSha256` TEXT) RETURNS INT
READS SQL DATA
RETURN (COALESCE((SELECT b.`BanSeconds` FROM `SoftwareBlocklist` b
    WHERE b.`Sha256` = lower(`PSha256`) AND b.`Enabled` AND b.`Severity` = 'ban'), 0));

-- bf-step view ClanActivityTotals
CREATE VIEW `ClanActivityTotals` AS
 SELECT `ClanId`,
    sum(`Points`) AS `TotalPoints`,
    (LEAST((2147483647), (1 + (sum(`Points`) DIV 10000)))) AS `ClanLevel`,
    ((sum(`Points`) % (10000))) AS `LevelPoints`
   FROM `ClanActivityGrants`
  GROUP BY `ClanId`;

-- bf-step view LifeGuardianPeriods
CREATE VIEW `LifeGuardianPeriods` AS
 SELECT `PlayerId`,
    `ItemInstanceId`,
    `EffectiveAt` AS `StartsAt`,
    GREATEST(`EffectiveAt`, LEAST(`ExpiresAt`, COALESCE(next_change, `ExpiresAt`))) AS `EndsAt`
   FROM ( SELECT `LifeGuardianEvents`.`EventId`,
            `LifeGuardianEvents`.`PlayerId`,
            `LifeGuardianEvents`.`ItemInstanceId`,
            `LifeGuardianEvents`.`EffectiveAt`,
            `LifeGuardianEvents`.`ExpiresAt`,
            `LifeGuardianEvents`.`EventKind`,
            lead(`LifeGuardianEvents`.`EffectiveAt`) OVER (PARTITION BY `LifeGuardianEvents`.`ItemInstanceId` ORDER BY `LifeGuardianEvents`.`EventId`) AS next_change
           FROM `LifeGuardianEvents`) events;

-- bf-step view NativeMedalProgress
CREATE VIEW `NativeMedalProgress` AS
 SELECT event.`PlayerId`,
    event.`MedalKey`,
    sum(event.`Increment`) AS `CumulativeCount`
   FROM ((`NativeMedalEvents` event
     JOIN `Matches` `Match` ON (((`Match`.`MatchId` = event.`MatchId`) AND (`Match`.`MatchStatus` = 'settled'))))
     JOIN `MatchSettlements` settlement ON (((settlement.`MatchId` = event.`MatchId`) AND (settlement.`SettlementStatus` = 'succeeded') AND (settlement.`ResultSnapshot` IS NOT NULL))))
  WHERE (`Match`.`ModeId` = (event.`MedalKey` DIV 10000))
  GROUP BY event.`PlayerId`, event.`MedalKey`;

-- bf-step view SinglePlayProgress
CREATE VIEW `SinglePlayProgress` AS
 SELECT `PlayerId`,
    `MissionKey`,
    `Directory`,
    `Difficulty`,
    `StageKey`,
    count(*) AS `Attempts`,
    COUNT(CASE WHEN (`SessionStatus` = 'reported_clear') THEN 1 END) AS `ReportedClears`,
    COUNT(CASE WHEN (`SessionStatus` = 'reported_failure') THEN 1 END) AS `ReportedFailures`,
    min(CASE WHEN (`SessionStatus` = 'reported_clear') THEN `ElapsedMs` END) AS `BestReportedElapsedMs`,
    'client_reported' AS `Authority`,
    'minimum_server_observed_playing_elapsed' AS `BestPolicy`
   FROM `SinglePlayAttempts`
  GROUP BY `PlayerId`, `MissionKey`, `Directory`, `Difficulty`, `StageKey`;

-- bf-step function EquipmentSlot
CREATE FUNCTION `EquipmentSlot`(`ItemId` BIGINT, `CatalogSlot` TEXT, `UseType` TEXT) RETURNS TEXT CHARACTER SET utf8mb4 COLLATE utf8mb4_bin
DETERMINISTIC NO SQL
RETURN (CASE
 WHEN `UseType`='10001' THEN 'pet'
 WHEN `ItemId` IN (100112,100412,100431,100432,100500,100546,100679,100986,101046,101069,101455,101786,101856,101862,101918,101923,101988,102116,102153,102154,190222) THEN 'crystal'
 WHEN `AccessoryMask`(`ItemId`)<>0 THEN CONCAT('accessory:', `AccessoryMask`(`ItemId`))
 ELSE `CatalogSlot` END);

-- bf-step view PlayerModerationState
CREATE VIEW `PlayerModerationState` AS
 SELECT p.`PlayerId`,
    a.`AccountId`,
    `AccountBanActive`(a.`AccountId`, a.`ActiveMachineCode`) AS `Banned`,
    (EXISTS ( SELECT 1
           FROM `ModerationSanctions` s
          WHERE ((s.`AccountId` = a.`AccountId`) AND (s.`Kind` = 'mute') AND (s.`RevokedAt` IS NULL) AND (s.`StartsAt` <= CURRENT_TIMESTAMP(6)) AND ((s.`ExpiresAt` IS NULL) OR (s.`ExpiresAt` > CURRENT_TIMESTAMP(6)))))) AS `Muted`
   FROM (`PlayerProfiles` p
     JOIN `Accounts` a USING (`AccountId`));

-- bf-step function ClothingCharacterCompatible
CREATE FUNCTION `ClothingCharacterCompatible`(`ItemId` BIGINT, `CharacterId` BIGINT) RETURNS BOOLEAN
DETERMINISTIC NO SQL
RETURN (CASE WHEN `ClothingCharacterMask`(`ItemId`) IS NULL THEN true
             WHEN `CharacterId` BETWEEN 300001 AND 300031
             THEN (`ClothingCharacterMask`(`ItemId`) & (1 << (`CharacterId`-300000)))<>0
             ELSE false END);

-- bf-step view NativeMedalBalances
CREATE VIEW `NativeMedalBalances` AS
 SELECT earned.`PlayerId`,
    earned.`MedalKey`,
    (earned.`CumulativeCount` - COALESCE(spent.`Amount`, (0))) AS `AvailableCount`
   FROM (`NativeMedalProgress` earned
     LEFT JOIN ( SELECT `NativeTitleMedalDebits`.`PlayerId`,
            `NativeTitleMedalDebits`.`MedalKey`,
            (sum(`NativeTitleMedalDebits`.`Amount`)) AS `Amount`
           FROM `NativeTitleMedalDebits`
          GROUP BY `NativeTitleMedalDebits`.`PlayerId`, `NativeTitleMedalDebits`.`MedalKey`) spent USING (`PlayerId`, `MedalKey`))
  WHERE ((earned.`CumulativeCount` - COALESCE(spent.`Amount`, (0))) > 0);

-- bf-step function PlayerChatDenied
CREATE FUNCTION `PlayerChatDenied`(`PPlayer` BIGINT) RETURNS BOOLEAN
READS SQL DATA
RETURN (COALESCE((SELECT `Banned` OR `Muted` FROM `PlayerModerationState` WHERE `PlayerId`=`PPlayer`),true));

-- bf-step trigger CkFriendCategoriesInsert
CREATE TRIGGER `CkFriendCategoriesInsert` AFTER INSERT ON `FriendCategories` FOR EACH ROW
BEGIN
IF NOT (NEW.`CategoryId` >= 1 AND NEW.`CategoryId` <= 2147483647) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 table constraint';
  END IF;
END;

-- bf-step trigger CkFriendCategoriesUpdate
CREATE TRIGGER `CkFriendCategoriesUpdate` AFTER UPDATE ON `FriendCategories` FOR EACH ROW
BEGIN
IF NOT (NEW.`CategoryId` >= 1 AND NEW.`CategoryId` <= 2147483647) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 table constraint';
  END IF;
END;

-- bf-step trigger CkGroupChatsInsert
CREATE TRIGGER `CkGroupChatsInsert` AFTER INSERT ON `GroupChats` FOR EACH ROW
BEGIN
IF NOT (NEW.`ChatId` >= 2147483648 AND NEW.`ChatId` <= 4294967294) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 table constraint';
  END IF;
END;

-- bf-step trigger CkGroupChatsUpdate
CREATE TRIGGER `CkGroupChatsUpdate` AFTER UPDATE ON `GroupChats` FOR EACH ROW
BEGIN
IF NOT (NEW.`ChatId` >= 2147483648 AND NEW.`ChatId` <= 4294967294) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 table constraint';
  END IF;
END;

-- bf-step trigger CkPlayerGiftsInsert
CREATE TRIGGER `CkPlayerGiftsInsert` AFTER INSERT ON `PlayerGifts` FOR EACH ROW
BEGIN
IF NOT (NEW.`GiftId` >= 1 AND NEW.`GiftId` <= 4294967295) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 table constraint';
  END IF;
END;

-- bf-step trigger CkPlayerGiftsUpdate
CREATE TRIGGER `CkPlayerGiftsUpdate` AFTER UPDATE ON `PlayerGifts` FOR EACH ROW
BEGIN
IF NOT (NEW.`GiftId` >= 1 AND NEW.`GiftId` <= 4294967295) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 table constraint';
  END IF;
END;

-- bf-step trigger CkPlayerChatBlocksInsert
CREATE TRIGGER `CkPlayerChatBlocksInsert` BEFORE INSERT ON `PlayerChatBlocks` FOR EACH ROW
BEGIN
IF NOT (NEW.`OwnerPlayerId` <> NEW.`BlockedPlayerId`) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 table constraint';
  END IF;
  IF NOT (NEW.`BlockedPlayerId` >= 1 AND NEW.`BlockedPlayerId` <= 4294967295) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 table constraint';
  END IF;
END;

-- bf-step trigger CkPlayerChatBlocksUpdate
CREATE TRIGGER `CkPlayerChatBlocksUpdate` BEFORE UPDATE ON `PlayerChatBlocks` FOR EACH ROW
BEGIN
IF NOT (NEW.`OwnerPlayerId` <> NEW.`BlockedPlayerId`) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 table constraint';
  END IF;
  IF NOT (NEW.`BlockedPlayerId` >= 1 AND NEW.`BlockedPlayerId` <= 4294967295) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 table constraint';
  END IF;
END;

-- bf-step trigger InventoryDiscardsUpdateGuard
CREATE TRIGGER `InventoryDiscardsUpdateGuard` BEFORE UPDATE ON `InventoryDiscards` FOR EACH ROW
BEGIN
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 historical ledger is append-only';
END;

-- bf-step trigger InventoryDiscardsDeleteGuard
CREATE TRIGGER `InventoryDiscardsDeleteGuard` BEFORE DELETE ON `InventoryDiscards` FOR EACH ROW
BEGIN
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 historical ledger is append-only';
END;

-- bf-step trigger LifeAccountingEpochUpdateGuard
CREATE TRIGGER `LifeAccountingEpochUpdateGuard` BEFORE UPDATE ON `LifeAccountingEpoch` FOR EACH ROW
BEGIN
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 historical ledger is append-only';
END;

-- bf-step trigger LifeAccountingEpochDeleteGuard
CREATE TRIGGER `LifeAccountingEpochDeleteGuard` BEFORE DELETE ON `LifeAccountingEpoch` FOR EACH ROW
BEGIN
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 historical ledger is append-only';
END;

-- bf-step trigger LifeAttendanceDaysUpdateGuard
CREATE TRIGGER `LifeAttendanceDaysUpdateGuard` BEFORE UPDATE ON `LifeAttendanceDays` FOR EACH ROW
BEGIN
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 historical ledger is append-only';
END;

-- bf-step trigger LifeAttendanceDaysDeleteGuard
CREATE TRIGGER `LifeAttendanceDaysDeleteGuard` BEFORE DELETE ON `LifeAttendanceDays` FOR EACH ROW
BEGIN
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 historical ledger is append-only';
END;

-- bf-step trigger LifeGuardianEventsUpdateGuard
CREATE TRIGGER `LifeGuardianEventsUpdateGuard` BEFORE UPDATE ON `LifeGuardianEvents` FOR EACH ROW
BEGIN
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 historical ledger is append-only';
END;

-- bf-step trigger LifeGuardianEventsDeleteGuard
CREATE TRIGGER `LifeGuardianEventsDeleteGuard` BEFORE DELETE ON `LifeGuardianEvents` FOR EACH ROW
BEGIN
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 historical ledger is append-only';
END;

-- bf-step trigger LifeItemSettlementsUpdateGuard
CREATE TRIGGER `LifeItemSettlementsUpdateGuard` BEFORE UPDATE ON `LifeItemSettlements` FOR EACH ROW
BEGIN
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 historical ledger is append-only';
END;

-- bf-step trigger LifeItemSettlementsDeleteGuard
CREATE TRIGGER `LifeItemSettlementsDeleteGuard` BEFORE DELETE ON `LifeItemSettlements` FOR EACH ROW
BEGIN
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 historical ledger is append-only';
END;

-- bf-step trigger NativeMedalEventsUpdateGuard
CREATE TRIGGER `NativeMedalEventsUpdateGuard` BEFORE UPDATE ON `NativeMedalEvents` FOR EACH ROW
BEGIN
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 historical ledger is append-only';
END;

-- bf-step trigger NativeMedalEventsDeleteGuard
CREATE TRIGGER `NativeMedalEventsDeleteGuard` BEFORE DELETE ON `NativeMedalEvents` FOR EACH ROW
BEGIN
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 historical ledger is append-only';
END;

-- bf-step trigger NativeTitleClaimsUpdateGuard
CREATE TRIGGER `NativeTitleClaimsUpdateGuard` BEFORE UPDATE ON `NativeTitleClaims` FOR EACH ROW
BEGIN
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 historical ledger is append-only';
END;

-- bf-step trigger NativeTitleClaimsDeleteGuard
CREATE TRIGGER `NativeTitleClaimsDeleteGuard` BEFORE DELETE ON `NativeTitleClaims` FOR EACH ROW
BEGIN
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 historical ledger is append-only';
END;

-- bf-step trigger NativeTitleMedalDebitsUpdateGuard
CREATE TRIGGER `NativeTitleMedalDebitsUpdateGuard` BEFORE UPDATE ON `NativeTitleMedalDebits` FOR EACH ROW
BEGIN
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 historical ledger is append-only';
END;

-- bf-step trigger NativeTitleMedalDebitsDeleteGuard
CREATE TRIGGER `NativeTitleMedalDebitsDeleteGuard` BEFORE DELETE ON `NativeTitleMedalDebits` FOR EACH ROW
BEGIN
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 historical ledger is append-only';
END;

-- bf-step trigger NativeTitleSelectionEventsUpdateGuard
CREATE TRIGGER `NativeTitleSelectionEventsUpdateGuard` BEFORE UPDATE ON `NativeTitleSelectionEvents` FOR EACH ROW
BEGIN
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 historical ledger is append-only';
END;

-- bf-step trigger NativeTitleSelectionEventsDeleteGuard
CREATE TRIGGER `NativeTitleSelectionEventsDeleteGuard` BEFORE DELETE ON `NativeTitleSelectionEvents` FOR EACH ROW
BEGIN
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 historical ledger is append-only';
END;

-- bf-step trigger PlayerGiftItemsUpdateGuard
CREATE TRIGGER `PlayerGiftItemsUpdateGuard` BEFORE UPDATE ON `PlayerGiftItems` FOR EACH ROW
BEGIN
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 historical ledger is append-only';
END;

-- bf-step trigger PlayerGiftItemsDeleteGuard
CREATE TRIGGER `PlayerGiftItemsDeleteGuard` BEFORE DELETE ON `PlayerGiftItems` FOR EACH ROW
BEGIN
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='r42 historical ledger is append-only';
END;

-- bf-step trigger HordeTutorHistoryDelete
CREATE TRIGGER `HordeTutorHistoryDelete` BEFORE DELETE ON `HordeTutorSessions` FOR EACH ROW
BEGIN
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Report history cannot be deleted';
END;

-- bf-step trigger HordeTutorHistoryUpdate
CREATE TRIGGER `HordeTutorHistoryUpdate` BEFORE UPDATE ON `HordeTutorSessions` FOR EACH ROW
BEGIN
IF NOT (NEW.`TutorialSessionId` <=> OLD.`TutorialSessionId`) OR NOT (NEW.`PlayerId` <=> OLD.`PlayerId`) OR NOT (NEW.`ModeId` <=> OLD.`ModeId`) OR NOT (NEW.`MapKey` <=> OLD.`MapKey`) OR NOT (NEW.`Difficulty` <=> OLD.`Difficulty`) OR NOT (NEW.`Authority` <=> OLD.`Authority`) OR NOT (NEW.`ProfileHash` <=> OLD.`ProfileHash`) OR NOT (NEW.`StartedAt` <=> OLD.`StartedAt`) OR NOT (NEW.`ExperienceAwarded` <=> OLD.`ExperienceAwarded`) OR NOT (NEW.`LucciAwarded` <=> OLD.`LucciAwarded`) OR NOT (NEW.`RatingDelta` <=> OLD.`RatingDelta`) OR NOT (NEW.`ItemAwards` <=> OLD.`ItemAwards`) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Report identity and zero-award fields are immutable';
  END IF;
  IF OLD.SessionStatus <> 'started' AND (NOT (NEW.`TutorialSessionId` <=> OLD.`TutorialSessionId`) OR NOT (NEW.`PlayerId` <=> OLD.`PlayerId`) OR NOT (NEW.`ModeId` <=> OLD.`ModeId`) OR NOT (NEW.`MapKey` <=> OLD.`MapKey`) OR NOT (NEW.`Difficulty` <=> OLD.`Difficulty`) OR NOT (NEW.`Authority` <=> OLD.`Authority`) OR NOT (NEW.`ProfileHash` <=> OLD.`ProfileHash`) OR NOT (NEW.`SessionStatus` <=> OLD.`SessionStatus`) OR NOT (NEW.`StartedAt` <=> OLD.`StartedAt`) OR NOT (NEW.`FinishedAt` <=> OLD.`FinishedAt`) OR NOT (NEW.`TerminalReason` <=> OLD.`TerminalReason`) OR NOT (NEW.`ExperienceAwarded` <=> OLD.`ExperienceAwarded`) OR NOT (NEW.`LucciAwarded` <=> OLD.`LucciAwarded`) OR NOT (NEW.`RatingDelta` <=> OLD.`RatingDelta`) OR NOT (NEW.`ItemAwards` <=> OLD.`ItemAwards`)) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Terminal report is immutable';
  END IF;
END;

-- bf-step trigger SinglePlayHistoryDelete
CREATE TRIGGER `SinglePlayHistoryDelete` BEFORE DELETE ON `SinglePlayAttempts` FOR EACH ROW
BEGIN
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Report history cannot be deleted';
END;

-- bf-step trigger SinglePlayHistoryUpdate
CREATE TRIGGER `SinglePlayHistoryUpdate` BEFORE UPDATE ON `SinglePlayAttempts` FOR EACH ROW
BEGIN
IF NOT (NEW.`AttemptId` <=> OLD.`AttemptId`) OR NOT (NEW.`PlayerId` <=> OLD.`PlayerId`) OR NOT (NEW.`ModeId` <=> OLD.`ModeId`) OR NOT (NEW.`MissionKey` <=> OLD.`MissionKey`) OR NOT (NEW.`Directory` <=> OLD.`Directory`) OR NOT (NEW.`Difficulty` <=> OLD.`Difficulty`) OR NOT (NEW.`StageKey` <=> OLD.`StageKey`) OR NOT (NEW.`Authority` <=> OLD.`Authority`) OR NOT (NEW.`ProfileHash` <=> OLD.`ProfileHash`) OR NOT (NEW.`StartedAt` <=> OLD.`StartedAt`) OR NOT (NEW.`ExperienceAwarded` <=> OLD.`ExperienceAwarded`) OR NOT (NEW.`LucciAwarded` <=> OLD.`LucciAwarded`) OR NOT (NEW.`RatingDelta` <=> OLD.`RatingDelta`) OR NOT (NEW.`ItemAwards` <=> OLD.`ItemAwards`) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Report identity and zero-award fields are immutable';
  END IF;
  IF OLD.SessionStatus <> 'started' AND (NOT (NEW.`AttemptId` <=> OLD.`AttemptId`) OR NOT (NEW.`PlayerId` <=> OLD.`PlayerId`) OR NOT (NEW.`ModeId` <=> OLD.`ModeId`) OR NOT (NEW.`MissionKey` <=> OLD.`MissionKey`) OR NOT (NEW.`Directory` <=> OLD.`Directory`) OR NOT (NEW.`Difficulty` <=> OLD.`Difficulty`) OR NOT (NEW.`StageKey` <=> OLD.`StageKey`) OR NOT (NEW.`Authority` <=> OLD.`Authority`) OR NOT (NEW.`ProfileHash` <=> OLD.`ProfileHash`) OR NOT (NEW.`SessionStatus` <=> OLD.`SessionStatus`) OR NOT (NEW.`StartedAt` <=> OLD.`StartedAt`) OR NOT (NEW.`FinishedAt` <=> OLD.`FinishedAt`) OR NOT (NEW.`TerminalReason` <=> OLD.`TerminalReason`) OR NOT (NEW.`ElapsedMs` <=> OLD.`ElapsedMs`) OR NOT (NEW.`ExperienceAwarded` <=> OLD.`ExperienceAwarded`) OR NOT (NEW.`LucciAwarded` <=> OLD.`LucciAwarded`) OR NOT (NEW.`RatingDelta` <=> OLD.`RatingDelta`) OR NOT (NEW.`ItemAwards` <=> OLD.`ItemAwards`)) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Terminal report is immutable';
  END IF;
END;

-- bf-step trigger FriendCategoriesRemoved
CREATE TRIGGER `FriendCategoriesRemoved` AFTER DELETE ON `Friendships` FOR EACH ROW
BEGIN
DELETE FROM FriendCategoryMembers
  WHERE (OwnerPlayerId=OLD.FirstPlayerId AND FriendPlayerId=OLD.SecondPlayerId)
     OR (OwnerPlayerId=OLD.SecondPlayerId AND FriendPlayerId=OLD.FirstPlayerId);
END;

-- bf-step trigger ClanLoginActivity
CREATE TRIGGER `ClanLoginActivity` AFTER INSERT ON `DailyLoginRewards` FOR EACH ROW
BEGIN
INSERT INTO ClanActivityGrants (ClanId,PlayerId,SourceKind,SourceId,Points,EarnedAt)
  SELECT c.ClanId,NEW.PlayerId,'login',NEW.BusinessId,20,NEW.ClaimedAt
  FROM ClanMemberships AS cm JOIN Clans AS c USING (ClanId)
  WHERE cm.PlayerId=NEW.PlayerId AND c.ClanKind='standard'
    AND c.ClanStatus IN ('active','pending_disband')
  ON DUPLICATE KEY UPDATE GrantId=ClanActivityGrants.GrantId;
END;

-- bf-step trigger ClanMatchActivity
CREATE TRIGGER `ClanMatchActivity` AFTER UPDATE ON `MatchSettlements` FOR EACH ROW
BEGIN
IF NEW.SettlementStatus='succeeded' AND OLD.SettlementStatus<>'succeeded'
     AND JSON_UNQUOTE(JSON_EXTRACT(NEW.TerminalSnapshot,'$.completion_kind'))='completed' THEN
    INSERT INTO ClanActivityGrants (ClanId,PlayerId,SourceKind,SourceId,Points,EarnedAt)
    SELECT cs.ClanId,p.PlayerId,'match',m.MatchId,
           ClanMatchActivityPoints(n.Members,p.Outcome,
             JSON_UNQUOTE(JSON_EXTRACT(m.RulesSnapshot,'$.clan_matchmaking'))='true'), m.EndedAt
    FROM Matches m JOIN ClanMatchSides cs USING (MatchId)
    JOIN JSON_TABLE(NEW.TerminalSnapshot,'$.participants[*]' COLUMNS (
      PlayerId BIGINT PATH '$.player_id', TeamCode INT PATH '$.team_code',
      Outcome VARCHAR(8) PATH '$.outcome', Departed INT PATH '$.departed' DEFAULT '0' ON EMPTY
    )) p ON p.TeamCode=cs.TeamCode
    JOIN LATERAL (SELECT COUNT(*) Members FROM MatchParticipants mp
      WHERE mp.MatchId=m.MatchId AND mp.TeamCode=cs.TeamCode) n ON TRUE
    WHERE m.MatchId=NEW.MatchId AND NOT COALESCE(p.Departed,0)
      AND ClanMatchActivityPoints(n.Members,p.Outcome,
             JSON_UNQUOTE(JSON_EXTRACT(m.RulesSnapshot,'$.clan_matchmaking'))='true') > 0
    ON DUPLICATE KEY UPDATE GrantId=ClanActivityGrants.GrantId;
  END IF;
END;

-- bf-step trigger NativeMedalEventInsertGuard
CREATE TRIGGER `NativeMedalEventInsertGuard` BEFORE INSERT ON `NativeMedalEvents` FOR EACH ROW
BEGIN
DECLARE ActualMode INT DEFAULT NULL;
  DECLARE ParticipantId BIGINT DEFAULT NULL;
  SELECT m.ModeId INTO ActualMode
    FROM Matches m JOIN MatchSettlements s USING (MatchId)
    WHERE m.MatchId=NEW.MatchId AND m.MatchStatus='settled'
      AND s.SettlementStatus='succeeded' AND s.ResultSnapshot IS NOT NULL FOR SHARE;
  IF ActualMode IS NULL OR ActualMode <> NEW.MedalKey DIV 10000 THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='native medal requires a succeeded settlement and matching mode';
  END IF;
  SELECT PlayerId INTO ParticipantId FROM MatchParticipants
    WHERE MatchId=NEW.MatchId AND PlayerId=NEW.PlayerId FOR UPDATE;
  IF ParticipantId IS NULL THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='native medal requires a real participant';
  END IF;
  IF EXISTS (SELECT 1 FROM NativeMedalEvents
      WHERE MatchId=NEW.MatchId AND PlayerId=NEW.PlayerId AND MedalKey=NEW.MedalKey
        AND (Increment<>NEW.Increment OR RuleEvidenceSha256<>NEW.RuleEvidenceSha256)) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='conflicting native medal replay';
  END IF;
END;

-- bf-step trigger NativeTitleEquipmentInsertGuard
CREATE TRIGGER `NativeTitleEquipmentInsertGuard` BEFORE INSERT ON `NativeTitleEquipment` FOR EACH ROW
BEGIN
DECLARE ItemId BIGINT DEFAULT NULL;
  DECLARE TitleKind INT DEFAULT NULL;
  SELECT i.ItemId,d.TitleType INTO ItemId,TitleKind FROM PlayerItems i
    JOIN NativeTitleDefinitions d ON d.TitleId=i.ItemId
    WHERE i.PlayerId=NEW.PlayerId AND i.ItemInstanceId=NEW.ItemInstanceId AND i.Quantity=1
      AND (i.ExpiresAt IS NULL OR i.ExpiresAt>SYSDATE(6)) FOR SHARE;
  IF ItemId IS NULL OR ItemId<>NEW.TitleId THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='native title requires owned active exact item';
  END IF;
  IF TitleKind=11 AND NOT EXISTS(SELECT 1 FROM CoupleMembers WHERE PlayerId=NEW.PlayerId) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='partner title requires active relationship';
  END IF;
END;

-- bf-step trigger NativeTitleEquipmentUpdateGuard
CREATE TRIGGER `NativeTitleEquipmentUpdateGuard` BEFORE UPDATE ON `NativeTitleEquipment` FOR EACH ROW
BEGIN
DECLARE ItemId BIGINT DEFAULT NULL;
  DECLARE TitleKind INT DEFAULT NULL;
  SELECT i.ItemId,d.TitleType INTO ItemId,TitleKind FROM PlayerItems i
    JOIN NativeTitleDefinitions d ON d.TitleId=i.ItemId
    WHERE i.PlayerId=NEW.PlayerId AND i.ItemInstanceId=NEW.ItemInstanceId AND i.Quantity=1
      AND (i.ExpiresAt IS NULL OR i.ExpiresAt>SYSDATE(6)) FOR SHARE;
  IF ItemId IS NULL OR ItemId<>NEW.TitleId THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='native title requires owned active exact item';
  END IF;
  IF TitleKind=11 AND NOT EXISTS(SELECT 1 FROM CoupleMembers WHERE PlayerId=NEW.PlayerId) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='partner title requires active relationship';
  END IF;
END;

-- bf-step trigger NativeTitleDebitGuard
CREATE TRIGGER `NativeTitleDebitGuard` BEFORE INSERT ON `NativeTitleMedalDebits` FOR EACH ROW
BEGIN
DECLARE ClaimOwner BIGINT DEFAULT NULL;
  DECLARE ClaimTitle INT DEFAULT NULL;
  DECLARE LockedPlayer BIGINT DEFAULT NULL;
  DECLARE Needed BIGINT DEFAULT NULL;
  DECLARE Available BIGINT DEFAULT NULL;
  SELECT PlayerId,TitleId INTO ClaimOwner,ClaimTitle FROM NativeTitleClaims WHERE ClaimId=NEW.ClaimId;
  IF ClaimOwner IS NULL OR ClaimOwner<>NEW.PlayerId THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='native title debit requires its authenticated claim';
  END IF;
  SELECT PlayerId INTO LockedPlayer FROM PlayerProfiles WHERE PlayerId=NEW.PlayerId FOR UPDATE;
  SELECT RequiredCount INTO Needed FROM NativeTitleRequirements WHERE TitleId=ClaimTitle AND MedalKey=NEW.MedalKey;
  SELECT AvailableCount INTO Available FROM NativeMedalBalances WHERE PlayerId=NEW.PlayerId AND MedalKey=NEW.MedalKey;
  IF Needed IS NULL OR Needed<>NEW.Amount OR COALESCE(Available,0)<NEW.Amount THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='native title cannot overspend or alter original requirement';
  END IF;
END;

-- bf-step trigger ItemAttributeInsertGuard
CREATE TRIGGER `ItemAttributeInsertGuard` BEFORE INSERT ON `PlayerItemAttributes` FOR EACH ROW
BEGIN
DECLARE OwnedPlayer BIGINT DEFAULT NULL;
  DECLARE OwnedItem BIGINT DEFAULT NULL;
  DECLARE OwnedQuantity BIGINT DEFAULT NULL;
  DECLARE OwnedExpiry DATETIME(6) DEFAULT NULL;
  DECLARE ItemSlot TEXT DEFAULT NULL;
  SELECT PlayerId,ItemId,Quantity,ExpiresAt INTO OwnedPlayer,OwnedItem,OwnedQuantity,OwnedExpiry
    FROM PlayerItems WHERE ItemInstanceId=NEW.ItemInstanceId FOR UPDATE;
  IF OwnedPlayer IS NULL OR OwnedPlayer<>NEW.PlayerId OR OwnedItem<>NEW.SourceItemId
     OR OwnedQuantity<>1 OR OwnedExpiry IS NOT NULL THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='attribute requires its exact permanent single inventory instance';
  END IF;
  SELECT EquipmentSlot(ItemId,SlotCode,JSON_UNQUOTE(JSON_EXTRACT(ClientFields,'$.use_type')))
    INTO ItemSlot FROM ItemTemplates WHERE ItemId=OwnedItem;
  IF ItemSlot IS NULL OR ItemSlot='' THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='attribute requires an equipment template';
  END IF;
END;

-- bf-step trigger ItemAttributeDeleteGuard
CREATE TRIGGER `ItemAttributeDeleteGuard` BEFORE DELETE ON `PlayerItemAttributes` FOR EACH ROW
BEGIN
SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Item attribute history is immutable';
END;

-- bf-step trigger ItemAttributeUpdateGuard
CREATE TRIGGER `ItemAttributeUpdateGuard` BEFORE UPDATE ON `PlayerItemAttributes` FOR EACH ROW
BEGIN
IF NOT (OLD.ItemInstanceId IS NOT NULL AND NEW.ItemInstanceId IS NULL AND @R42DeletingItemInstance <=> OLD.ItemInstanceId AND (NEW.`AttributeId` <=> OLD.`AttributeId`) AND (NEW.`PlayerId` <=> OLD.`PlayerId`) AND (NEW.`SourceItemId` <=> OLD.`SourceItemId`) AND (NEW.`DefinitionId` <=> OLD.`DefinitionId`) AND (NEW.`AttributeValues` <=> OLD.`AttributeValues`) AND (NEW.`Category` <=> OLD.`Category`) AND (NEW.`Label` <=> OLD.`Label`) AND (NEW.`CreatedAt` <=> OLD.`CreatedAt`)) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Item attributes cannot be changed or rebound';
  END IF;
END;

-- bf-step trigger AttributedInventoryUpdateGuard
CREATE TRIGGER `AttributedInventoryUpdateGuard` BEFORE UPDATE ON `PlayerItems` FOR EACH ROW
BEGIN
IF EXISTS (SELECT 1 FROM PlayerItemAttributes WHERE ItemInstanceId=OLD.ItemInstanceId) AND (NEW.ItemInstanceId<>OLD.ItemInstanceId OR NEW.PlayerId<>OLD.PlayerId OR NEW.ItemId<>OLD.ItemId OR NEW.Quantity<>1 OR NEW.ExpiresAt IS NOT NULL) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Attributed inventory identity must be preserved';
  END IF;
END;

-- bf-step trigger LifeInventoryDelete
CREATE TRIGGER `LifeInventoryDelete` BEFORE DELETE ON `PlayerItems` FOR EACH ROW
BEGIN
DECLARE Stamp DATETIME(6) DEFAULT SYSDATE(6);
  DECLARE EXIT HANDLER FOR SQLEXCEPTION BEGIN
    SET @R42DeletingItemInstance = NULL;
    RESIGNAL;
  END;
  SET @R42DeletingItemInstance = OLD.ItemInstanceId;
  UPDATE PlayerItemAttributes SET ItemInstanceId=NULL WHERE ItemInstanceId=OLD.ItemInstanceId;
  SET @R42DeletingItemInstance = NULL;
  UPDATE PlayerPetGrowth SET ItemInstanceId=NULL WHERE ItemInstanceId=OLD.ItemInstanceId;
  UPDATE InventoryTransactions SET ItemInstanceId=NULL WHERE ItemInstanceId=OLD.ItemInstanceId;
  IF IsLifeItem(OLD.ItemId) THEN
    DELETE FROM LifeItemCursors WHERE ItemInstanceId=OLD.ItemInstanceId;
  END IF;
  IF OLD.ItemId=102141 THEN
    INSERT INTO LifeGuardianEvents (PlayerId,ItemInstanceId,EffectiveAt,ExpiresAt,EventKind)
    VALUES (OLD.PlayerId,OLD.ItemInstanceId,Stamp,Stamp,'remove');
  END IF;
END;

-- bf-step trigger LifeInventoryInsert
CREATE TRIGGER `LifeInventoryInsert` AFTER INSERT ON `PlayerItems` FOR EACH ROW
BEGIN
DECLARE Stamp DATETIME(6) DEFAULT SYSDATE(6);
  IF IsLifeItem(NEW.ItemId) THEN
    INSERT INTO LifeItemCursors (ItemInstanceId,NextDate)
    VALUES (NEW.ItemInstanceId,DATE_ADD(GREATEST(DATE(NEW.AcquiredAt+INTERVAL 8 HOUR),
          DATE(Stamp+INTERVAL 8 HOUR)),INTERVAL 1 DAY));
  END IF;
  IF NEW.ItemId=102141 THEN
    IF NEW.ExpiresAt IS NULL OR NEW.Quantity<>1 THEN
      SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Life guardian requires one timed instance';
    END IF;
    INSERT INTO LifeGuardianEvents (PlayerId,ItemInstanceId,EffectiveAt,ExpiresAt,EventKind)
    VALUES (NEW.PlayerId,NEW.ItemInstanceId,Stamp,NEW.ExpiresAt,'grant');
  END IF;
END;

-- bf-step trigger LifeInventoryUpdate
CREATE TRIGGER `LifeInventoryUpdate` BEFORE UPDATE ON `PlayerItems` FOR EACH ROW
BEGIN
IF (NEW.PlayerId<>OLD.PlayerId OR NEW.ItemId<>OLD.ItemId OR NEW.ItemInstanceId<>OLD.ItemInstanceId)
     AND (IsLifeItem(NEW.ItemId) OR IsLifeItem(OLD.ItemId) OR NEW.ItemId=102141 OR OLD.ItemId=102141) THEN
    SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Life instance identity cannot change';
  END IF;
  IF NEW.ItemId=102141 THEN
    IF NEW.ExpiresAt IS NULL OR NEW.Quantity<>1 THEN
      SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='Life guardian requires one timed instance';
    END IF;
    IF NOT (NEW.ExpiresAt <=> OLD.ExpiresAt) THEN
      INSERT INTO LifeGuardianEvents (PlayerId,ItemInstanceId,EffectiveAt,ExpiresAt,EventKind)
      VALUES (NEW.PlayerId,NEW.ItemInstanceId,SYSDATE(6),NEW.ExpiresAt,'change');
    END IF;
  END IF;
END;
