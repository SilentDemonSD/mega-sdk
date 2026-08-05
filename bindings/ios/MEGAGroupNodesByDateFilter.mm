/**
 * @file MEGAGroupNodesByDateFilter.mm
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
#import "MEGAGroupNodesByDateFilter.h"

NS_ASSUME_NONNULL_BEGIN

@implementation MEGAGroupNodesByDateFilter

- (instancetype)init {
    self = [super init];
    if (self != nil) {
        _category = MEGANodeFormatTypeUnknown;
        _locationHandles = nil;
        _excludeLocationHandles = nil;
        _location = MEGAListAllNodesFilterLocationCloudDriveAndVault;
        _sensitivityFilter = MEGAListAllNodesFilterSensitivityOptionDisabled;
        _granularity = MEGAGroupNodesByDateGranularityMonth;
        _utcOffset = nil;
    }
    return self;
}

@end

NS_ASSUME_NONNULL_END
