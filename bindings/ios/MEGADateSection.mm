/**
 * @file MEGADateSection.mm
 * @brief One date bucket of nodes returned by [MEGASdk groupAllNodesByDateWithFilter:...].
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
#import "MEGADateSection.h"
#import "megaapi.h"

using namespace mega;

@interface MEGADateSection ()

@property MegaDateSection *dateSection;
@property BOOL cMemoryOwn;

@end

@implementation MEGADateSection

- (instancetype)initWithDateSection:(MegaDateSection *)dateSection cMemoryOwn:(BOOL)cMemoryOwn {
    self = [super init];

    if (self != nil) {
        _dateSection = dateSection;
        _cMemoryOwn = cMemoryOwn;
    }

    return self;
}

- (void)dealloc {
    if (self.cMemoryOwn) {
        delete _dateSection;
    }
}

- (MegaDateSection *)getCPtr {
    return self.dateSection;
}

- (nullable NSString *)groupId {
    if (self.dateSection == NULL) {
        return nil;
    }

    const char *groupId = self.dateSection->getGroupId();
    if (groupId == NULL) {
        return nil;
    }

    return [NSString stringWithUTF8String:groupId];
}

- (int64_t)startDate {
    return self.dateSection ? self.dateSection->getStartDate() : 0;
}

- (int64_t)endDate {
    return self.dateSection ? self.dateSection->getEndDate() : 0;
}

- (int64_t)count {
    return self.dateSection ? self.dateSection->getCount() : 0;
}

@end
