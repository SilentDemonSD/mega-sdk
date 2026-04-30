/**
 * @file MEGAFileServiceStorageInfo.mm
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
#import "MEGAFileServiceStorageInfo.h"
#import "megaapi.h"

using namespace mega;

@interface MEGAFileServiceStorageInfo ()

@property MegaFileServiceStorageInfo *info;
@property BOOL cMemoryOwn;

@end

@implementation MEGAFileServiceStorageInfo

- (instancetype)initWithMegaFileServiceStorageInfo:(MegaFileServiceStorageInfo *)info cMemoryOwn:(BOOL)cMemoryOwn {
    self = [super init];

    if (self != nil) {
        _info = info;
        _cMemoryOwn = cMemoryOwn;
    }

    return self;
}

- (MegaFileServiceStorageInfo *)getCPtr {
    return self.info;
}

- (void)dealloc {
    if (self.cMemoryOwn) {
        delete _info;
    }
}

- (uint64_t)allocatedSize {
    return self.info ? self.info->getAllocatedSize() : 0;
}

- (uint64_t)reclaimableSize {
    return self.info ? self.info->getReclaimableSize() : 0;
}

@end
