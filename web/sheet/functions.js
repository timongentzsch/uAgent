// The functions the engine calculates, as a formula being typed is offered
// them: a name, its arguments (in brackets those that may be left out, "…"
// where more may follow), and what it gives.
export const FUNCTIONS = `
ABS(number) A number without its sign
ACCRINT(issue, first_interest, settlement, rate, par, frequency, [basis], [calc_method]) Interest accrued on a security that pays interest periodically
ACCRINTM(issue, settlement, rate, [par], [basis]) Interest accrued on a security that pays interest at maturity
ACOS(number) The angle, in radians, whose cosine is a number
ACOSH(number) Inverse hyperbolic cosine
ACOT(number) The angle, in radians, whose cotangent is a number
ACOTH(number) Inverse hyperbolic cotangent
ADDRESS(row_num, column_num, [abs_num], [a1], [sheet_text]) A cell's address as text, from its row and column numbers
AMORDEGRC(cost, date_purchased, first_period, salvage, period, rate, [basis]) Depreciation for a period, by a coefficient that depends on the asset's life
AMORLINC(cost, date_purchased, first_period, salvage, period, rate, [basis]) Depreciation for a period, the same amount each full period
AND(logical1, [logical2], …) TRUE when every condition is true
ARABIC(text) A Roman numeral as a number
AREAS(reference) How many areas a reference has
ARRAYTOTEXT(array, [format]) The values of a range as one text
ASC(text) Full-width characters as half-width ones
ASIN(number) The angle, in radians, whose sine is a number
ASINH(number) Inverse hyperbolic sine
ATAN(number) The angle, in radians, whose tangent is a number
ATAN2(x_num, y_num) The angle, in radians, from the x-axis to a point
ATANH(number) Inverse hyperbolic tangent
AVEDEV(number1, [number2], …) How far numbers lie from their mean, on average
AVERAGE(number1, [number2], …) The mean of numbers
AVERAGEA(value1, [value2], …) The mean of values, with text as 0 and TRUE as 1
AVERAGEIF(range, criteria, [average_range]) The mean of the cells that meet a condition
AVERAGEIFS(average_range, criteria_range1, criteria1, …) The mean of the cells that meet every condition
BASE(number, radix, [min_length]) A number as text in another base
BESSELI(x, n) The modified Bessel function In(x)
BESSELJ(x, n) The Bessel function Jn(x)
BESSELK(x, n) The modified Bessel function Kn(x)
BESSELY(x, n) The Bessel function Yn(x)
BETA.DIST(x, alpha, beta, cumulative, [a], [b]) The beta distribution
BETA.INV(probability, alpha, beta, [a], [b]) The inverse of the cumulative beta distribution
BETADIST(x, alpha, beta, [a], [b]) The cumulative beta distribution
BETAINV(probability, alpha, beta, [a], [b]) The inverse of the cumulative beta distribution
BIN2DEC(number) A binary number as decimal
BIN2HEX(number, [places]) A binary number as hexadecimal
BIN2OCT(number, [places]) A binary number as octal
BINOM.DIST(number_s, trials, probability_s, cumulative) The chance of a number of successes in a number of trials
BINOM.DIST.RANGE(trials, probability_s, number_s, [number_s2]) The chance that the successes lie between two numbers
BINOM.INV(trials, probability_s, alpha) The fewest successes whose cumulative chance reaches a level
BINOMDIST(number_s, trials, probability_s, cumulative) The chance of a number of successes in a number of trials
BITAND(number1, number2) Two numbers combined bit by bit with AND
BITLSHIFT(number, shift_amount) A number with its bits moved left
BITOR(number1, number2) Two numbers combined bit by bit with OR
BITRSHIFT(number, shift_amount) A number with its bits moved right
BITXOR(number1, number2) Two numbers combined bit by bit with exclusive OR
BYCOL(array, lambda) What a LAMBDA gives for each column
BYROW(array, lambda) What a LAMBDA gives for each row
CEILING(number, significance) A number rounded up to a multiple
CEILING.MATH(number, [significance], [mode]) A number rounded up to a whole number or a multiple
CEILING.PRECISE(number, [significance]) A number rounded up to a multiple, whatever its sign
CELL(info_type, [reference]) Something about a cell: its address, row, column or contents
CHAR(number) The character with a code number
CHIDIST(x, deg_freedom) The right-tailed chance of the chi-squared distribution
CHIINV(probability, deg_freedom) The inverse of the right-tailed chi-squared chance
CHISQ.DIST(x, deg_freedom, cumulative) The chi-squared distribution
CHISQ.DIST.RT(x, deg_freedom) The right-tailed chance of the chi-squared distribution
CHISQ.INV(probability, deg_freedom) The inverse of the left-tailed chi-squared chance
CHISQ.INV.RT(probability, deg_freedom) The inverse of the right-tailed chi-squared chance
CHISQ.TEST(actual_range, expected_range) The chi-squared test for independence
CHITEST(actual_range, expected_range) The chi-squared test for independence
CHOOSE(index_num, value1, [value2], …) The value at a place in a list
CHOOSECOLS(array, col_num1, [col_num2], …) The columns of a range picked by number
CHOOSEROWS(array, row_num1, [row_num2], …) The rows of a range picked by number
CLEAN(text) Text without the characters that cannot be printed
CODE(text) The code number of a text's first character
COLUMN([reference]) The number of a cell's column
COLUMNS(array) How many columns a range has
COMBIN(number, number_chosen) How many ways items can be chosen, in any order
COMBINA(number, number_chosen) How many ways items can be chosen, in any order, with repeats
COMPLEX(real_num, i_num, [suffix]) A complex number from its real and imaginary parts
CONCAT(text1, [text2], …) Texts and ranges joined into one text
CONCATENATE(text1, [text2], …) Texts joined into one
CONFIDENCE(alpha, standard_dev, size) The confidence interval of a mean, by the normal distribution
CONFIDENCE.NORM(alpha, standard_dev, size) The confidence interval of a mean, by the normal distribution
CONFIDENCE.T(alpha, standard_dev, size) The confidence interval of a mean, by Student's t-distribution
CONVERT(number, from_unit, to_unit) A number in another unit of measure
CORREL(array1, array2) How closely two sets of values go together
COS(number) The cosine of an angle in radians
COSH(number) Hyperbolic cosine
COT(number) The cotangent of an angle in radians
COTH(number) Hyperbolic cotangent
COUNT(value1, [value2], …) How many cells hold numbers
COUNTA(value1, [value2], …) How many cells are not empty
COUNTBLANK(range) How many cells are empty
COUNTIF(range, criteria) How many cells meet a condition
COUNTIFS(criteria_range1, criteria1, …) How many rows meet every condition
COUPDAYBS(settlement, maturity, frequency, [basis]) Days from the start of the coupon period to settlement
COUPDAYS(settlement, maturity, frequency, [basis]) Days in the coupon period that holds the settlement date
COUPDAYSNC(settlement, maturity, frequency, [basis]) Days from settlement to the next coupon date
COUPNCD(settlement, maturity, frequency, [basis]) The next coupon date after settlement
COUPNUM(settlement, maturity, frequency, [basis]) How many coupons are paid between settlement and maturity
COUPPCD(settlement, maturity, frequency, [basis]) The last coupon date before settlement
COVAR(array1, array2) The covariance of two sets, as whole populations
COVARIANCE.P(array1, array2) The covariance of two sets, as whole populations
COVARIANCE.S(array1, array2) The covariance of two sets, as samples
CRITBINOM(trials, probability_s, alpha) The fewest successes whose cumulative chance reaches a level
CSC(number) The cosecant of an angle in radians
CSCH(number) Hyperbolic cosecant
CUMIPMT(rate, nper, pv, start_period, end_period, type) The interest paid on a loan between two periods
CUMPRINC(rate, nper, pv, start_period, end_period, type) The principal paid on a loan between two periods
DATE(year, month, day) A date from its year, month and day
DATEDIF(start_date, end_date, unit) The days, months or years between two dates
DATEVALUE(date_text) A date written as text, as a date
DAVERAGE(database, field, criteria) The mean of a column in the records that match
DAY(serial_number) The day of the month of a date
DAYS(end_date, start_date) The days between two dates
DAYS360(start_date, end_date, [method]) The days between two dates in a year of 360 days
DB(cost, salvage, life, period, [month]) Depreciation for a period, by a fixed declining balance
DCOUNT(database, field, criteria) How many matching records hold a number in a column
DCOUNTA(database, field, criteria) How many matching records are not empty in a column
DDB(cost, salvage, life, period, [factor]) Depreciation for a period, by a double declining balance
DEC2BIN(number, [places]) A decimal number as binary
DEC2HEX(number, [places]) A decimal number as hexadecimal
DEC2OCT(number, [places]) A decimal number as octal
DECIMAL(text, radix) Text in another base as a decimal number
DEGREES(angle) Radians as degrees
DELTA(number1, [number2]) 1 when two numbers are equal, 0 when not
DEVSQ(number1, [number2], …) The sum of squared distances from the mean
DGET(database, field, criteria) The value of a column in the one record that matches
DISC(settlement, maturity, pr, redemption, [basis]) The discount rate of a security
DMAX(database, field, criteria) The largest number of a column in the records that match
DMIN(database, field, criteria) The smallest number of a column in the records that match
DOLLAR(number, [decimals]) A number as text in currency format
DOLLARDE(fractional_dollar, fraction) A price written as a fraction, as a decimal
DOLLARFR(decimal_dollar, fraction) A decimal price written as a fraction
DPRODUCT(database, field, criteria) The product of a column in the records that match
DROP(array, rows, [columns]) A range without rows or columns at its start or end
DSTDEV(database, field, criteria) The standard deviation of a column in matching records, as a sample
DSTDEVP(database, field, criteria) The standard deviation of a column in matching records, as a population
DSUM(database, field, criteria) The sum of a column in the records that match
DURATION(settlement, maturity, coupon, yld, frequency, [basis]) The Macaulay duration of a security
DVAR(database, field, criteria) The variance of a column in matching records, as a sample
DVARP(database, field, criteria) The variance of a column in matching records, as a population
ECMA.CEILING(number, significance) A number rounded up to a multiple
EDATE(start_date, months) The date a number of months before or after a date
EFFECT(nominal_rate, npery) The effective yearly interest rate
ENCODEURL(text) Text encoded for a web address
EOMONTH(start_date, months) The last day of the month, a number of months away
ERF(lower_limit, [upper_limit]) The error function
ERF.PRECISE(x) The error function from 0 to a number
ERFC(x) The complementary error function
ERFC.PRECISE(x) The complementary error function
ERROR.TYPE(error_val) A number for the kind of an error
EVEN(number) A number rounded away from zero to an even whole number
EXACT(text1, text2) TRUE when two texts are the same, capitals too
EXP(number) e raised to a power
EXPAND(array, rows, [columns], [pad_with]) A range grown to a number of rows and columns
EXPON.DIST(x, lambda, cumulative) The exponential distribution
EXPONDIST(x, lambda, cumulative) The exponential distribution
F.DIST(x, deg_freedom1, deg_freedom2, cumulative) The F distribution
F.DIST.RT(x, deg_freedom1, deg_freedom2) The right-tailed chance of the F distribution
F.INV(probability, deg_freedom1, deg_freedom2) The inverse of the F distribution
F.INV.RT(probability, deg_freedom1, deg_freedom2) The inverse of the right-tailed F chance
F.TEST(array1, array2) The chance that two sets have the same variance
FACT(number) The factorial of a number
FACTDOUBLE(number) The double factorial of a number
FDIST(x, deg_freedom1, deg_freedom2) The right-tailed chance of the F distribution
FILTER(array, include, [if_empty]) The rows or columns that meet a condition
FIND(find_text, within_text, [start_num]) Where one text starts in another, capitals mattering
FINDB(find_text, within_text, [start_num]) Where one text starts in another, counted in bytes
FINV(probability, deg_freedom1, deg_freedom2) The inverse of the right-tailed F chance
FISHER(x) The Fisher transformation
FISHERINV(y) The inverse of the Fisher transformation
FIXED(number, [decimals], [no_commas]) A number as text with a set number of decimals
FLOOR(number, significance) A number rounded down to a multiple
FLOOR.MATH(number, [significance], [mode]) A number rounded down to a whole number or a multiple
FLOOR.PRECISE(number, [significance]) A number rounded down to a multiple, whatever its sign
FORECAST(x, known_ys, known_xs) A value on the straight line through known values
FORECAST.LINEAR(x, known_ys, known_xs) A value on the straight line through known values
FORMULATEXT(reference) A cell's formula as text
FREQUENCY(data_array, bins_array) How many values fall into each interval
FTEST(array1, array2) The chance that two sets have the same variance
FV(rate, nper, pmt, [pv], [type]) What an investment is worth after its payments
FVSCHEDULE(principal, schedule) What a sum is worth after a series of interest rates
GAMMA(number) The gamma function
GAMMA.DIST(x, alpha, beta, cumulative) The gamma distribution
GAMMA.INV(probability, alpha, beta) The inverse of the cumulative gamma distribution
GAMMADIST(x, alpha, beta, cumulative) The gamma distribution
GAMMAINV(probability, alpha, beta) The inverse of the cumulative gamma distribution
GAMMALN(x) The natural logarithm of the gamma function
GAMMALN.PRECISE(x) The natural logarithm of the gamma function
GAUSS(z) The chance that a standard normal value lies between the mean and z
GCD(number1, [number2], …) The greatest common divisor
GEOMEAN(number1, [number2], …) The geometric mean
GESTEP(number, [step]) 1 when a number is at least a step, 0 when not
GROWTH(known_ys, [known_xs], [new_xs], [const]) Values on the exponential curve through known values
HARMEAN(number1, [number2], …) The harmonic mean
HEX2BIN(number, [places]) A hexadecimal number as binary
HEX2DEC(number) A hexadecimal number as decimal
HEX2OCT(number, [places]) A hexadecimal number as octal
HLOOKUP(lookup_value, table_array, row_index_num, [range_lookup]) Looks along the top row and gives the value below, from a row
HOUR(serial_number) The hour of a time
HSTACK(array1, [array2], …) Ranges set side by side
HYPERLINK(link_location, [friendly_name]) A link to an address, shown as a label
HYPGEOM.DIST(sample_s, number_sample, population_s, number_pop, cumulative) The hypergeometric distribution
HYPGEOMDIST(sample_s, number_sample, population_s, number_pop) The hypergeometric distribution
IF(logical_test, value_if_true, [value_if_false]) One value when a condition is true, another when not
IFERROR(value, value_if_error) A value, or another when it is an error
IFNA(value, value_if_na) A value, or another when it is #N/A
IFS(logical_test1, value_if_true1, …) The value of the first condition that is true
IMABS(inumber) The absolute value of a complex number
IMAGINARY(inumber) The imaginary part of a complex number
IMARGUMENT(inumber) The angle of a complex number, in radians
IMCONJUGATE(inumber) The conjugate of a complex number
IMCOS(inumber) The cosine of a complex number
IMCOSH(inumber) The hyperbolic cosine of a complex number
IMCOT(inumber) The cotangent of a complex number
IMCSC(inumber) The cosecant of a complex number
IMCSCH(inumber) The hyperbolic cosecant of a complex number
IMDIV(inumber1, inumber2) One complex number divided by another
IMEXP(inumber) e raised to a complex number
IMLN(inumber) The natural logarithm of a complex number
IMLOG10(inumber) The base-10 logarithm of a complex number
IMLOG2(inumber) The base-2 logarithm of a complex number
IMPOWER(inumber, number) A complex number raised to a power
IMPRODUCT(inumber1, inumber2) Two complex numbers multiplied
IMREAL(inumber) The real part of a complex number
IMSEC(inumber) The secant of a complex number
IMSECH(inumber) The hyperbolic secant of a complex number
IMSIN(inumber) The sine of a complex number
IMSINH(inumber) The hyperbolic sine of a complex number
IMSQRT(inumber) The square root of a complex number
IMSUB(inumber1, inumber2) One complex number less another
IMSUM(inumber1, inumber2) Two complex numbers added
IMTAN(inumber) The tangent of a complex number
INDEX(array, row_num, [column_num], [area_num]) The value at a row and column of a range
INDIRECT(ref_text, [a1]) The cell a text names
INFO(type_text) Something about where the sheet runs
INT(number) A number rounded down to a whole number
INTERCEPT(known_ys, known_xs) Where the straight line through known values meets the y-axis
INTRATE(settlement, maturity, investment, redemption, [basis]) The interest rate of a fully invested security
IPMT(rate, per, nper, pv, [fv], [type]) The interest part of a loan payment in a period
IRR(values, [guess]) The rate of return of regular cash flows
ISBLANK(value) TRUE for an empty cell
ISERR(value) TRUE for any error but #N/A
ISERROR(value) TRUE for any error
ISEVEN(number) TRUE for an even number
ISFORMULA(reference) TRUE for a cell that holds a formula
ISLOGICAL(value) TRUE for TRUE or FALSE
ISNA(value) TRUE for the error #N/A
ISNONTEXT(value) TRUE for anything but text
ISNUMBER(value) TRUE for a number
ISO.CEILING(number, [significance]) A number rounded up to a multiple, whatever its sign
ISODD(number) TRUE for an odd number
ISOMITTED(argument) TRUE when a LAMBDA was called without an argument
ISOWEEKNUM(date) The ISO week of the year of a date
ISPMT(rate, per, nper, pv) The interest of a period on a loan repaid in equal parts
ISREF(value) TRUE for a reference
ISTEXT(value) TRUE for text
KURT(number1, [number2], …) How peaked a set of values is
LAMBDA(parameter_or_calculation, …) A function of your own: its parameters, then what it calculates
LARGE(array, k) The k-th largest value
LCM(number1, [number2], …) The least common multiple
LEFT(text, [num_chars]) The first characters of a text
LEFTB(text, [num_bytes]) The first characters of a text, counted in bytes
LEN(text) How many characters a text has
LENB(text) How many bytes a text has
LET(name1, name_value1, calculation_or_name2, …) Names values, then calculates with the names
LINEST(known_ys, [known_xs], [const], [stats]) The straight line that best fits known values
LN(number) The natural logarithm
LOG(number, [base]) The logarithm to a base, 10 when none is given
LOG10(number) The base-10 logarithm
LOGEST(known_ys, [known_xs], [const], [stats]) The exponential curve that best fits known values
LOGINV(probability, mean, standard_dev) The inverse of the cumulative lognormal distribution
LOGNORM.DIST(x, mean, standard_dev, cumulative) The lognormal distribution
LOGNORM.INV(probability, mean, standard_dev) The inverse of the cumulative lognormal distribution
LOGNORMDIST(x, mean, standard_dev) The cumulative lognormal distribution
LOOKUP(lookup_value, lookup_vector, [result_vector]) Looks in one row or column and gives the value at the same place in another
LOWER(text) Text in small letters
MAKEARRAY(rows, cols, lambda) A range of a size, each cell what a LAMBDA gives
MAP(array1, lambda_or_array2, …) What a LAMBDA gives for each value of a range
MATCH(lookup_value, lookup_array, [match_type]) Where a value lies in a row or column
MAX(number1, [number2], …) The largest number
MAXA(value1, [value2], …) The largest value, with text as 0 and TRUE as 1
MAXIFS(max_range, criteria_range1, criteria1, …) The largest of the cells that meet every condition
MDETERM(array) The determinant of a matrix
MDURATION(settlement, maturity, coupon, yld, frequency, [basis]) The modified duration of a security
MEDIAN(number1, [number2], …) The middle number
MID(text, start_num, num_chars) Characters from the middle of a text
MIDB(text, start_num, num_bytes) Characters from the middle of a text, counted in bytes
MIN(number1, [number2], …) The smallest number
MINA(value1, [value2], …) The smallest value, with text as 0 and TRUE as 1
MINIFS(min_range, criteria_range1, criteria1, …) The smallest of the cells that meet every condition
MINUTE(serial_number) The minute of a time
MINVERSE(array) The inverse of a matrix
MIRR(values, finance_rate, reinvest_rate) The rate of return with different rates for borrowing and reinvesting
MMULT(array1, array2) The product of two matrices
MOD(number, divisor) What is left after a division
MODE(number1, [number2], …) The most frequent number
MODE.MULT(number1, [number2], …) The most frequent numbers
MODE.SNGL(number1, [number2], …) The most frequent number
MONTH(serial_number) The month of a date
MROUND(number, multiple) A number rounded to a multiple
MULTINOMIAL(number1, [number2], …) The factorial of a sum over the product of the factorials
MUNIT(dimension) The unit matrix of a size
N(value) A value as a number
NA() The error #N/A, for a value that is not there
NEGBINOM.DIST(number_f, number_s, probability_s, cumulative) The negative binomial distribution
NEGBINOMDIST(number_f, number_s, probability_s) The negative binomial distribution
NETWORKDAYS(start_date, end_date, [holidays]) The working days between two dates
NETWORKDAYS.INTL(start_date, end_date, [weekend], [holidays]) The working days between two dates, with the weekend you name
NOMINAL(effect_rate, npery) The nominal yearly interest rate
NORM.DIST(x, mean, standard_dev, cumulative) The normal distribution
NORM.INV(probability, mean, standard_dev) The inverse of the cumulative normal distribution
NORM.S.DIST(z, cumulative) The standard normal distribution
NORM.S.INV(probability) The inverse of the cumulative standard normal distribution
NORMDIST(x, mean, standard_dev, cumulative) The normal distribution
NORMINV(probability, mean, standard_dev) The inverse of the cumulative normal distribution
NORMSDIST(z) The cumulative standard normal distribution
NORMSINV(probability) The inverse of the cumulative standard normal distribution
NOT(logical) TRUE for FALSE, and FALSE for TRUE
NOW() The date and time now
NPER(rate, pmt, pv, [fv], [type]) How many periods an investment takes
NPV(rate, value1, [value2], …) What future cash flows are worth today
NUMBERVALUE(text, [decimal_separator], [group_separator]) Text as a number, with the separators you name
OCT2BIN(number, [places]) An octal number as binary
OCT2DEC(number) An octal number as decimal
OCT2HEX(number, [places]) An octal number as hexadecimal
ODD(number) A number rounded away from zero to an odd whole number
ODDFPRICE(settlement, maturity, issue, first_coupon, rate, yld, redemption, frequency, [basis]) The price of a security with an odd first period
ODDFYIELD(settlement, maturity, issue, first_coupon, rate, pr, redemption, frequency, [basis]) The yield of a security with an odd first period
ODDLPRICE(settlement, maturity, last_interest, rate, yld, redemption, frequency, [basis]) The price of a security with an odd last period
ODDLYIELD(settlement, maturity, last_interest, rate, pr, redemption, frequency, [basis]) The yield of a security with an odd last period
OFFSET(reference, rows, cols, [height], [width]) A range a number of rows and columns away from a cell
OR(logical1, [logical2], …) TRUE when any condition is true
PDURATION(rate, pv, fv) How many periods an investment takes to reach a value
PEARSON(array1, array2) The Pearson correlation of two sets of values
PERCENTILE(array, k) The value below which a share of the values lies
PERCENTILE.EXC(array, k) The value below which a share of the values lies, 0 and 1 left out
PERCENTILE.INC(array, k) The value below which a share of the values lies
PERCENTOF(data_subset, data_all) What share a part is of the whole
PERCENTRANK(array, x, [significance]) The rank of a value in a set, as a share
PERCENTRANK.EXC(array, x, [significance]) The rank of a value in a set, as a share, 0 and 1 left out
PERCENTRANK.INC(array, x, [significance]) The rank of a value in a set, as a share
PERMUT(number, number_chosen) How many ways items can be chosen, in order
PERMUTATIONA(number, number_chosen) How many ways items can be chosen, in order, with repeats
PHI(x) The density of the standard normal distribution
PI() The number π
PMT(rate, nper, pv, [fv], [type]) The payment of a loan each period
POISSON(x, mean, cumulative) The Poisson distribution
POISSON.DIST(x, mean, cumulative) The Poisson distribution
POWER(number, power) A number raised to a power
PPMT(rate, per, nper, pv, [fv], [type]) The principal part of a loan payment in a period
PRICE(settlement, maturity, rate, yld, redemption, frequency, [basis]) The price of a security that pays interest periodically
PRICEDISC(settlement, maturity, discount, redemption, [basis]) The price of a discounted security
PRICEMAT(settlement, maturity, issue, rate, yld, [basis]) The price of a security that pays interest at maturity
PROB(x_range, prob_range, lower_limit, [upper_limit]) The chance that values lie between two limits
PRODUCT(number1, [number2], …) Numbers multiplied
PROPER(text) Text with a capital at the start of each word
PV(rate, nper, pmt, [fv], [type]) What an investment is worth today
QUARTILE(array, quart) A quartile of a set of values
QUARTILE.EXC(array, quart) A quartile of a set of values, 0 and 1 left out
QUARTILE.INC(array, quart) A quartile of a set of values
QUOTIENT(numerator, denominator) The whole part of a division
RADIANS(angle) Degrees as radians
RAND() A random number from 0 up to 1
RANDARRAY([rows], [columns], [min], [max], [whole_number]) A range of random numbers
RANDBETWEEN(bottom, top) A random whole number between two numbers
RANK(number, ref, [order]) The rank of a number in a list
RANK.AVG(number, ref, [order]) The rank of a number in a list, the average rank for a tie
RANK.EQ(number, ref, [order]) The rank of a number in a list, the top rank for a tie
RATE(nper, pmt, pv, [fv], [type]) The interest rate per period of a loan or an investment
RECEIVED(settlement, maturity, investment, discount, [basis]) What a fully invested security pays at maturity
REDUCE([initial_value], array, lambda) One value built up by a LAMBDA over each value of a range
REGEXEXTRACT(text, pattern, [return_mode]) The part of a text that matches a pattern
REGEXREPLACE(text, pattern, replacement) Text with what matches a pattern replaced
REGEXTEST(text, pattern) TRUE when a text matches a pattern
REPLACE(old_text, start_num, num_chars, new_text) Text with the characters at a place replaced
REPLACEB(old_text, start_num, num_bytes, new_text) Text with the characters at a place replaced, counted in bytes
REPT(text, number_times) Text repeated a number of times
RIGHT(text, [num_chars]) The last characters of a text
RIGHTB(text, [num_bytes]) The last characters of a text, counted in bytes
ROMAN(number, [form]) A number as a Roman numeral
ROUND(number, num_digits) A number rounded to a number of digits
ROUNDDOWN(number, num_digits) A number rounded toward zero
ROUNDUP(number, num_digits) A number rounded away from zero
ROW([reference]) The number of a cell's row
ROWS(array) How many rows a range has
RRI(nper, pv, fv) The interest rate at which an investment grows to a value
RSQ(known_ys, known_xs) The square of the Pearson correlation
SCAN([initial_value], array, lambda) Each step of a value built up by a LAMBDA over a range
SEARCH(find_text, within_text, [start_num]) Where one text starts in another, capitals not mattering
SEARCHB(find_text, within_text, [start_num]) Where one text starts in another, counted in bytes
SEC(number) The secant of an angle in radians
SECH(number) Hyperbolic secant
SECOND(serial_number) The second of a time
SEQUENCE(rows, [columns], [start], [step]) A range of numbers in order
SERIESSUM(x, n, m, coefficients) The sum of a power series
SHEET([value]) The number of a tab
SHEETS([reference]) How many tabs there are
SIGN(number) 1 for a positive number, -1 for a negative one, 0 for zero
SIN(number) The sine of an angle in radians
SINH(number) Hyperbolic sine
SKEW(number1, [number2], …) How lopsided a set of values is, as a sample
SKEW.P(number1, [number2], …) How lopsided a set of values is, as a population
SLN(cost, salvage, life) Depreciation for one period, the same each period
SLOPE(known_ys, known_xs) The slope of the straight line through known values
SMALL(array, k) The k-th smallest value
SORT(array, [sort_index], [sort_order], [by_col]) A range in order
SORTBY(array, by_array1, [sort_order1], …) A range in the order of another
SQRT(number) The square root
SQRTPI(number) The square root of a number times π
STANDARDIZE(x, mean, standard_dev) How many standard deviations a value lies from the mean
STDEV(number1, [number2], …) The standard deviation of a sample
STDEV.P(number1, [number2], …) The standard deviation of a population
STDEV.S(number1, [number2], …) The standard deviation of a sample
STDEVA(value1, [value2], …) The standard deviation of a sample, with text as 0 and TRUE as 1
STDEVP(number1, [number2], …) The standard deviation of a population
STDEVPA(value1, [value2], …) The standard deviation of a population, with text as 0 and TRUE as 1
STEYX(known_ys, known_xs) The standard error of the y a straight line predicts
SUBSTITUTE(text, old_text, new_text, [instance_num]) Text with one piece replaced by another
SUBTOTAL(function_num, ref1, [ref2], …) A sum, a count or another total of a range, by its number
SUM(number1, [number2], …) Numbers added
SUMIF(range, criteria, [sum_range]) The sum of the cells that meet a condition
SUMIFS(sum_range, criteria_range1, criteria1, …) The sum of the cells that meet every condition
SUMPRODUCT(array1, [array2], …) Ranges multiplied cell by cell, then added
SUMSQ(number1, [number2], …) The sum of the squares
SUMX2MY2(array_x, array_y) The sum of the differences of the squares of two ranges
SUMX2PY2(array_x, array_y) The sum of the sums of the squares of two ranges
SUMXMY2(array_x, array_y) The sum of the squared differences of two ranges
SWITCH(expression, value1, result1, …) The result that goes with the first value an expression equals
SYD(cost, salvage, life, per) Depreciation for a period, by the sum of the years' digits
T(value) A value if it is text, nothing if not
T.DIST(x, deg_freedom, cumulative) Student's t-distribution, left-tailed
T.DIST.2T(x, deg_freedom) Student's t-distribution, two-tailed
T.DIST.RT(x, deg_freedom) Student's t-distribution, right-tailed
T.INV(probability, deg_freedom) The inverse of the left-tailed t-distribution
T.INV.2T(probability, deg_freedom) The inverse of the two-tailed t-distribution
T.TEST(array1, array2, tails, type) The chance that two sets have the same mean, by Student's t-test
TAKE(array, rows, [columns]) The rows or columns at the start or end of a range
TAN(number) The tangent of an angle in radians
TANH(number) Hyperbolic tangent
TBILLEQ(settlement, maturity, discount) The bond-equivalent yield of a Treasury bill
TBILLPRICE(settlement, maturity, discount) The price of a Treasury bill
TBILLYIELD(settlement, maturity, pr) The yield of a Treasury bill
TDIST(x, deg_freedom, tails) Student's t-distribution
TEXT(value, format_text) A number as text in a format
TEXTAFTER(text, delimiter, [instance_num], [match_mode], [match_end], [if_not_found]) The text after a delimiter
TEXTBEFORE(text, delimiter, [instance_num], [match_mode], [match_end], [if_not_found]) The text before a delimiter
TEXTJOIN(delimiter, ignore_empty, text1, [text2], …) Texts joined with a delimiter between them
TEXTSPLIT(text, col_delimiter, [row_delimiter], [ignore_empty], [match_mode], [pad_with]) Text split into cells at a delimiter
TIME(hour, minute, second) A time from its hour, minute and second
TIMEVALUE(time_text) A time written as text, as a time
TINV(probability, deg_freedom) The inverse of the two-tailed t-distribution
TOCOL(array, [ignore], [scan_by_column]) A range as one column
TODAY() The date today
TOROW(array, [ignore], [scan_by_column]) A range as one row
TRANSPOSE(array) A range with its rows and columns swapped
TREND(known_ys, [known_xs], [new_xs], [const]) Values on the straight line through known values
TRIM(text) Text without spaces at its ends or doubled between words
TRIMMEAN(array, percent) The mean without a share of the highest and lowest values
TRIMRANGE(range, [trim_rows], [trim_cols]) A range without the empty rows and columns at its edges
TRUNC(number, [num_digits]) A number cut to a number of digits
TTEST(array1, array2, tails, type) The chance that two sets have the same mean, by Student's t-test
TYPE(value) A number for the kind of a value
UNICHAR(number) The character with a Unicode number
UNICODE(text) The Unicode number of a text's first character
UNIQUE(array, [by_col], [exactly_once]) The different values of a range
UPPER(text) Text in capitals
VALUE(text) Text as a number
VALUETOTEXT(value) A value as text
VAR(number1, [number2], …) The variance of a sample
VAR.P(number1, [number2], …) The variance of a population
VAR.S(number1, [number2], …) The variance of a sample
VARA(value1, [value2], …) The variance of a sample, with text as 0 and TRUE as 1
VARP(number1, [number2], …) The variance of a population
VARPA(value1, [value2], …) The variance of a population, with text as 0 and TRUE as 1
VDB(cost, salvage, life, start_period, end_period, [factor], [no_switch]) Depreciation between two periods, by a declining balance
VLOOKUP(lookup_value, table_array, col_index_num, [range_lookup]) Looks down the first column and gives the value beside, from a column
VSTACK(array1, [array2], …) Ranges set one under another
WEEKDAY(serial_number, [return_type]) The day of the week of a date, as a number
WEEKNUM(serial_number, [return_type]) The week of the year of a date
WEIBULL(x, alpha, beta, cumulative) The Weibull distribution
WEIBULL.DIST(x, alpha, beta, cumulative) The Weibull distribution
WORKDAY(start_date, days, [holidays]) The date a number of working days away
WORKDAY.INTL(start_date, days, [weekend], [holidays]) The date a number of working days away, with the weekend you name
WRAPCOLS(vector, wrap_count, [pad_with]) One row or column wrapped into columns of a length
WRAPROWS(vector, wrap_count, [pad_with]) One row or column wrapped into rows of a length
XIRR(values, dates, [guess]) The rate of return of cash flows on their dates
XLOOKUP(lookup_value, lookup_array, return_array, [if_not_found], [match_mode], [search_mode]) Looks in one range and gives the value at the same place in another
XMATCH(lookup_value, lookup_array, [match_mode], [search_mode]) Where a value lies in a row or column
XNPV(rate, values, dates) What cash flows on their dates are worth today
XOR(logical1, [logical2], …) TRUE when an odd number of conditions are true
YEAR(serial_number) The year of a date
YEARFRAC(start_date, end_date, [basis]) The share of a year between two dates
YIELD(settlement, maturity, rate, pr, redemption, frequency, [basis]) The yield of a security that pays interest periodically
YIELDDISC(settlement, maturity, pr, redemption, [basis]) The yearly yield of a discounted security
YIELDMAT(settlement, maturity, issue, rate, pr, [basis]) The yearly yield of a security that pays interest at maturity
Z.TEST(array, x, [sigma]) The one-tailed chance of a z-test
ZTEST(array, x, [sigma]) The one-tailed chance of a z-test
`;
