//
//  MariaDBUnitTests.m
//  MariaDBUnitTests
//
//  Created by Daniel Parnell on 6/12/12.
//  Copyright (c) 2012 Automagic Software Pty Ltd. All rights reserved.
//

#import "MariaDBUnitTests.h"
#import "MariaDB.h"

@implementation MariaDBUnitTests

- (void)setUp
{
    [super setUp];
    
    // Set-up code here.
}

- (void)tearDown
{
    // Tear-down code here.
    
    [super tearDown];
}

- (void)testConnect
{
    NSError* error = nil;
    MariaDB* db = [[MariaDB alloc] initWithHost: @"ubuntudev.local"
                                           user: @"test_user"
                                       password: @"test_password"
                                       database: @"test_database"
                                           port: 3306
                                         socket: nil
                                          flags: 0
                                       andError: &error];
    
    STAssertNotNil(db, [error domain]);
}

@end
