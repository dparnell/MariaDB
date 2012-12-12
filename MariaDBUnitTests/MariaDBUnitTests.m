//
//  MariaDBUnitTests.m
//  MariaDBUnitTests
//
//  Created by Daniel Parnell on 6/12/12.
//  Copyright (c) 2012 Automagic Software Pty Ltd.
/*
 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Library General Public
 License as published by the Free Software Foundation; either
 version 2 of the License, or (at your option) any later version.
 
 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Library General Public License for more details.
 
 You should have received a copy of the GNU Library General Public
 License along with this library; if not, write to the Free
 Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 MA 02111-1307, USA
 */
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

- (void) testQuery {
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
    
    [db execute: @"DROP TABLE IF EXISTS query_test" withError: &error];
    STAssertNil(error, [error domain]);
    
    [db execute: @"CREATE TABLE query_test (id int not null, my_name varchar(50), PRIMARY KEY(id))" withError: &error];
    STAssertNil(error, [error domain]);
    
    [db execute: @"INSERT INTO query_test VALUES (1, \"First value\"), (2, \"Second value\")" withError: &error];
    STAssertNil(error, [error domain]);
    
    STAssertEquals([db affectedRows], (UInt64)2, @"It should insert 2 rows");
    
    NSMutableArray* rows = [NSMutableArray new];
    if(![db query: @"select * from query_test order by id" withCallback:^BOOL(NSDictionary *row, NSArray* fields) {
        [rows addObject: row];
        
        STAssertEqualObjects(@"id", [fields objectAtIndex: 0], @"it should be 'id'");
        STAssertEqualObjects(@"my_name", [fields objectAtIndex: 1], @"it should be 'my_name'");
        return YES;
    } andError: &error]) {
        STFail([error domain]);
    }
    
    STAssertEquals((int)[rows count], 2, @"it should return 2 rows");
    NSDictionary* row1 = [rows objectAtIndex: 0];
    STAssertEqualObjects([NSNumber numberWithInt: 1], [row1 objectForKey: @"id"], @"the id of the first row should be the NSNumber 1");
    STAssertEqualObjects(@"First value", [row1 objectForKey: @"my_name"], @"the value of the 'my_name' field in the first row should be 'First value'");
    

    rows = [NSMutableArray new];
    if(![db query: @"select current_timestamp" withCallback:^BOOL(NSDictionary *row, NSArray* fields) {
        [rows addObject: row];
        
        STAssertEqualObjects(@"current_timestamp", [fields objectAtIndex: 0], @"it should be 'current_timestamp'");
        return YES;
    } andError: &error]) {
        STFail([error domain]);
    }
    id value = [[rows objectAtIndex: 0] objectForKey: @"current_timestamp"];
    STAssertTrue([value isKindOfClass: [NSDate class]], [NSString stringWithFormat: @"it should be a date - %@", value]);

    rows = [NSMutableArray new];
    if(![db query: @"select current_date" withCallback:^BOOL(NSDictionary *row, NSArray* fields) {
        [rows addObject: row];
        
        STAssertEqualObjects(@"current_date", [fields objectAtIndex: 0], @"it should be 'current_date'");
        return YES;
    } andError: &error]) {
        STFail([error domain]);
    }
    value = [[rows objectAtIndex: 0] objectForKey: @"current_date"];
    STAssertTrue([value isKindOfClass: [NSDate class]], [NSString stringWithFormat: @"it should be a date - %@", value]);

    rows = [NSMutableArray new];
    if(![db query: @"select current_time" withCallback:^BOOL(NSDictionary *row, NSArray* fields) {
        [rows addObject: row];
        
        STAssertEqualObjects(@"current_time", [fields objectAtIndex: 0], @"it should be 'current_time'");
        return YES;
    } andError: &error]) {
        STFail([error domain]);
    }
    value = [[rows objectAtIndex: 0] objectForKey: @"current_time"];
    STAssertTrue([value isKindOfClass: [NSDate class]], [NSString stringWithFormat: @"it should be a date - %@", value]);

    STAssertFalse([db execute: @"select * from foobar" withError: &error], @"the select should have failed");
    error = nil;
    
    [db close: &error];
    STAssertNil(error, [error domain]);
}

@end
