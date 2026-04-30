/**
 * @file MEGAFileServiceStorageInfo.h
 * @brief Storage size information about MEGA's file services.
 *
 * (c) 2013-2014 by Mega Limited, Auckland, New Zealand
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
 * @brief Storage size information about MEGA's file services.
 *
 * Use [MEGASdk fileServiceGetStorageInfo:] to retrieve a snapshot of the current state.
 */
@interface MEGAFileServiceStorageInfo : NSObject

/**
 * @brief The size that is currently allocated by the file service in bytes.
 */
@property (readonly, nonatomic) uint64_t allocatedSize;

/**
 * @brief The size that can be reclaimed by the file service in bytes.
 *
 * The reclaimable space is up to the reclaim option settings.
 */
@property (readonly, nonatomic) uint64_t reclaimableSize;

@end

NS_ASSUME_NONNULL_END
