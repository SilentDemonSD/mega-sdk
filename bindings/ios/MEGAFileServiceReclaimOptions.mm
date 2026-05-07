/**
 * @file MEGAFileServiceReclaimOptions.mm
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
#import "MEGAFileServiceReclaimOptions.h"
#import "megaapi.h"

using namespace mega;

@interface MEGAFileServiceReclaimOptions ()

@property MegaFileServiceReclaimOptions *options;
@property BOOL cMemoryOwn;

@end

@implementation MEGAFileServiceReclaimOptions

- (instancetype)init {
    MegaFileServiceReclaimOptions *cOptions = MegaFileServiceReclaimOptions::create();
    return [self initWithMegaFileServiceReclaimOptions:cOptions cMemoryOwn:YES];
}

- (instancetype)initWithMegaFileServiceReclaimOptions:(MegaFileServiceReclaimOptions *)options cMemoryOwn:(BOOL)cMemoryOwn {
    self = [super init];

    if (self != nil) {
        _options = options;
        _cMemoryOwn = cMemoryOwn;
    }

    return self;
}

- (MegaFileServiceReclaimOptions *)getCPtr {
    return self.options;
}

- (void)dealloc {
    if (self.cMemoryOwn) {
        delete _options;
    }
}

- (NSInteger)ageThreshold {
    return self.options ? self.options->getAgeThreshold() : 0;
}

- (void)setAgeThreshold:(NSInteger)ageThreshold {
    if (self.options) {
        self.options->setAgeThreshold((int)ageThreshold);
    }
}

- (NSUInteger)batchSize {
    return self.options ? self.options->getBatchSize() : 0;
}

- (void)setBatchSize:(NSUInteger)batchSize {
    if (self.options) {
        self.options->setBatchSize((std::size_t)batchSize);
    }
}

- (uint64_t)delay {
    return self.options ? self.options->getDelay() : 0;
}

- (void)setDelay:(uint64_t)delay {
    if (self.options) {
        self.options->setDelay(delay);
    }
}

- (uint64_t)period {
    return self.options ? self.options->getPeriod() : 0;
}

- (void)setPeriod:(uint64_t)period {
    if (self.options) {
        self.options->setPeriod(period);
    }
}

- (int64_t)reclaimThreshold {
    return self.options ? self.options->getReclaimThreshold() : 0;
}

- (void)setReclaimThreshold:(int64_t)reclaimThreshold {
    if (self.options) {
        self.options->setReclaimThreshold(reclaimThreshold);
    }
}

- (uint64_t)reclaimTarget {
    return self.options ? self.options->getReclaimTarget() : 0;
}

- (void)setReclaimTarget:(uint64_t)reclaimTarget {
    if (self.options) {
        self.options->setReclaimTarget(reclaimTarget);
    }
}

@end
