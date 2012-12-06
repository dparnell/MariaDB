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
    [db close: &error];
    STAssertNil(error, [error domain]);
}

- (void) testExecute
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
    
    [db execute: @"DROP TABLE IF EXISTS affected_rows" withError: &error];
    STAssertNil(error, [error domain]);

    [db execute: @"CREATE TABLE affected_rows (id int not null, my_name varchar(50), PRIMARY KEY(id))" withError: &error];
    STAssertNil(error, [error domain]);

    [db execute: @"INSERT INTO affected_rows VALUES (1, \"First value\"), (2, \"Second value\")" withError: &error];
    STAssertNil(error, [error domain]);

    STAssertEquals([db affectedRows], (UInt64)2, @"It should insert 2 rows");
    
    
    [db close: &error];
    STAssertNil(error, [error domain]);
}
@end
