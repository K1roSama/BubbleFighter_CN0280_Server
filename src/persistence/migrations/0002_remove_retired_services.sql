-- Remove retired email verification and anti-cheat storage.
-- Device identities, device bans, account sanctions and audit history remain.

-- bf-step function SoftwareBlocklistAction
DROP FUNCTION IF EXISTS `SoftwareBlocklistAction`;

-- bf-step function SoftwareBlocklistBanSeconds
DROP FUNCTION IF EXISTS `SoftwareBlocklistBanSeconds`;

-- bf-step table EmailBindingSessions
DROP TABLE IF EXISTS `EmailBindingSessions`;

-- bf-step table EmailVerificationChallenges
DROP TABLE IF EXISTS `EmailVerificationChallenges`;

-- bf-step table AccountEmails
DROP TABLE IF EXISTS `AccountEmails`;

-- bf-step table ForensicsAttachments
DROP TABLE IF EXISTS `ForensicsAttachments`;

-- bf-step table ForensicsReports
DROP TABLE IF EXISTS `ForensicsReports`;

-- bf-step table SoftwareDetections
DROP TABLE IF EXISTS `SoftwareDetections`;

-- bf-step table SoftwareBlocklist
DROP TABLE IF EXISTS `SoftwareBlocklist`;
