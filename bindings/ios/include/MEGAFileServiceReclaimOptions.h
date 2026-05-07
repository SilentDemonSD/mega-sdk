/**
 * @file MEGAFileServiceReclaimOptions.h
 * @brief Options to control how MEGA's file services reclaim local storage.
 *
 * (c) 2026- by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/**
 * @brief Options to control how MEGA's file services reclaim local storage.
 *
 * Use [MEGASdk fileServiceGetStorageInfo:] and [MEGASdk fileServiceReclaim:delegate:] to inspect
 * or trigger reclaim using these options.
 */
@interface MEGAFileServiceReclaimOptions : NSObject

/**
 * @brief Create a new options instance with default values.
 */
- (instancetype)init;

/**
 * @brief The reclaim age threshold in minutes.
 *
 * How old should the files be before they are considered for reclamation?
 */
@property (nonatomic) NSInteger ageThreshold;

/**
 * @brief The reclaim batch size.
 *
 * How many files should be processed in each batch of reclaim process?
 * This size is rarely needed to be changed.
 */
@property (nonatomic) NSUInteger batchSize;

/**
 * @brief The reclaim delay in seconds.
 *
 * How long after startup should we wait until we reclaim space?
 */
@property (nonatomic) uint64_t delay;

/**
 * @brief The reclaim period in seconds.
 *
 * How long should we wait between consecutive reclaims?
 */
@property (nonatomic) uint64_t period;

/**
 * @brief The reclaim trigger threshold in bytes.
 *
 * The threshold in bytes that, when the used space exceeds it (and other required conditions are
 * satisfied), may trigger a reclaim operation. Reclaiming stops once usage falls below this
 * threshold.
 *
 * - 0  : No minimum threshold (reclaim may run immediately).
 * - -1 : Automatic reclamation is disabled.
 */
@property (nonatomic) int64_t reclaimThreshold;

/**
 * @brief The reclaim target size in bytes.
 *
 * The target size in bytes that the used space should be reduced to when a reclaim operation is
 * triggered.
 */
@property (nonatomic) uint64_t reclaimTarget;

@end

NS_ASSUME_NONNULL_END
