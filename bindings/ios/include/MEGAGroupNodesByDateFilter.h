/**
 * @file MEGAGroupNodesByDateFilter.h
 * @brief Filter for [MEGASdk groupAllNodesByDateWithFilter:...] queries.
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
#import "MEGAListAllNodesFilter.h"

/**
 * @brief Bucket granularity for [MEGASdk groupAllNodesByDateWithFilter:...].
 *
 * groupAllNodesByDate returns an empty list (and logs a warning) for an
 * out-of-range value.
 */
typedef NS_ENUM (NSInteger, MEGAGroupNodesByDateGranularity) {
    MEGAGroupNodesByDateGranularityDay = 0,   ///< Group id "YYYY-MM-DD".
    MEGAGroupNodesByDateGranularityMonth = 1, ///< Group id "YYYY-MM" (default).
    MEGAGroupNodesByDateGranularityYear = 2   ///< Group id "YYYY".
};

NS_ASSUME_NONNULL_BEGIN

/**
 * @brief Filter for [MEGASdk groupAllNodesByDateWithFilter:...].
 *
 * Carries the same node-selection scope as MEGAListAllNodesFilter (mime
 * category, rootnode/location scope, ancestor include/exclude handles and
 * sensitivity) and adds the section granularity. It deliberately does NOT
 * expose a timestamp anchor: grouping has no pagination anchor and always
 * returns the section list across the entire filter scope.
 *
 * File versions are always excluded regardless of scope.
 */
@interface MEGAGroupNodesByDateFilter : NSObject

/**
 * @brief Required. MIME type category (see MEGANodeFormatType in MEGASearchFilter.h).
 *
 * Must be a non-default value. groupAllNodesByDate rejects
 * MEGANodeFormatTypeUnknown (returns an empty list and logs a warning).
 */
@property (nonatomic) MEGANodeFormatType category;

/**
 * @brief Optional. Restrict results to descendants of one or more ancestor
 * handles (max `MEGAListAllNodesFilterMaxLocationHandles`, currently 3).
 *
 * Leave nil or empty (default) to use the rootnode scope chosen by `location`.
 */
@property (nonatomic, copy, nullable) NSArray<NSNumber *> *locationHandles;

/**
 * @brief Optional. Drop nodes whose ancestor chain contains any of the supplied
 * handles (max `MEGAListAllNodesFilterMaxLocationHandles`).
 *
 * Applied independently of `locationHandles` / `location`. Pass nil or an empty
 * array (default) to disable.
 */
@property (nonatomic, copy, nullable) NSArray<NSNumber *> *excludeLocationHandles;

/**
 * @brief Optional. Rootnode scope selector. Ignored when `locationHandles` is
 * set (non-empty).
 *
 * Default: MEGAListAllNodesFilterLocationCloudDriveAndVault.
 */
@property (nonatomic) MEGAListAllNodesFilterLocation location;

/**
 * @brief Optional. Sensitivity filter.
 *
 * Default: MEGAListAllNodesFilterSensitivityOptionDisabled (no filtering).
 */
@property (nonatomic) MEGAListAllNodesFilterSensitivityOption sensitivityFilter;

/**
 * @brief Optional. Bucket granularity.
 *
 * Default: MEGAGroupNodesByDateGranularityMonth.
 */
@property (nonatomic) MEGAGroupNodesByDateGranularity granularity;

- (instancetype)init;

@end

NS_ASSUME_NONNULL_END
