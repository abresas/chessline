#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_SIZE 256
// backtrack needed because what may look like a departure position may be destination position
#define BACKTRACK_SIZE 8 
#define ERROR_MESSAGE_SIZE 256

typedef enum {white, black} playerSide;
typedef enum {pawn, knight, bishop, rook, queen, king} pieceEnum;
typedef enum {aFile = 1, bFile = 2, cFile = 3, dFile = 4, eFile = 5, fFile = 6, gFile = 7, hFile = 8} chessFile;

typedef struct {
    chessFile file;
    int rank;
} position;

typedef struct moveTag {
    position departurePosition;
    pieceEnum piece;
    pieceEnum promoteTo;
    position destination;
    int probability;
    struct moveTag* firstChoice;
    struct moveTag* nextChoice;
    struct moveTag* previousMove;
    bool isCapture;
    bool isCheck;
    bool isCheckmate;
    bool isRoot;
    bool isShortCastling;
    bool isLongCastling;
    int decisionLevel;
} move;

move* mkMove() {
    move* m = (move*)malloc(sizeof(move));
    m->firstChoice = NULL;
    m->nextChoice = NULL;
    m->previousMove = NULL;
    m->departurePosition.file = 0;
    m->departurePosition.rank = 0;
    m->promoteTo = pawn;
    m->probability = 0;
    m->isCapture = m->isCheck = m->isCheckmate = m->isRoot = m->isShortCastling = m->isLongCastling = m->decisionLevel = 0;
    return m;
}

void appendMove(move* previous, move* next) {
    next->previousMove = previous;
    if (!previous->firstChoice) {
        previous->firstChoice = next;
    } else {
        move* lastChoice = previous->firstChoice;
        while (lastChoice->nextChoice != NULL) {
            lastChoice = lastChoice->nextChoice;
        }
        lastChoice->nextChoice = next;
    }
}

typedef struct {
    bool hasError;
    char errorMessage[ERROR_MESSAGE_SIZE];
    char actualCharacter;
    union {
        int integer;
        int digit;
        chessFile file;
        int rank;
        pieceEnum piece;
        char character;
    };
} readResult;

typedef struct {
    bool hasError;
    union {
        char errorMessage[ERROR_MESSAGE_SIZE];
        move* moveTreeRoot;
    };
} parseResult;

typedef enum {
    startState, beginParseMoveState, parseProbabilityState, parseMovePieceState, parseDepartureFileState, parseDepartureRankState,
    parseMoveCaptureState, parseMoveDestinationState, parseMovePromotionState, parseMoveCheckState, parseMoveCheckmateState,
    parseAlgebraicNotation, parseShortCastlingState, parseLongCastlingState,
    finishParseMoveState, parseWhitespaceState, parseIndentationLevelState,
    parseDepartureOrDestinationState, noDepartureState, endState
} parserState;

static const char* stateStrings[] = {
    "start", "start_parse_move", "parse_probability", "parse_move_piece", "parse_departure_file", "parse_departure_rank",
    "parse_capture", "parse_destination", "parse_promotion", "parse_check", "parse_checkmate",
    "start_algebraic_notation_move", "parse_short_castling", "parse_long_castling",
    "end_parse_move", "parse_whitespace", "parse_indentation_level",
    "departure_or_destination", "no_departure", "end"
};


const char* state_to_string(parserState state) {
    // fprintf(stderr, "state_to_string(%d)\n", state);
    return stateStrings[state];
}

typedef struct {
    int initialCharacterCount;
    parserState stateOnFailure;
    int line;
    int column;
} parserAttempt;

typedef struct {
    int top;
    parserAttempt array[2];
} parserAttemptStack;

parserAttempt* pop_attempt(parserAttemptStack* attemptStack) {
    // fprintf(stderr, "pop_attempt() (top=%d)\n", attemptStack->top);
    if (attemptStack->top == -1) {
        return NULL;
    }
    return &attemptStack->array[attemptStack->top--];
}

void push_attempt(parserAttemptStack* attemptStack, parserAttempt attempt) {
    //fprintf(stderr, "push_attempt() (top=%d)\n", attemptStack->top);
    attemptStack->array[++attemptStack->top] = attempt;
}

typedef struct {
    FILE* file;
    int line;
    int column;
    char buffer[BUFFER_SIZE];
    int bufferSize;
    int bufferCursor; // offset from start of buffer that we have already read
    char errorMessage[ERROR_MESSAGE_SIZE];
    int decisionLevel;
    parserState state;
    move* currentMove;
    move* moveTree;
    int totalCharacterCount;
    parserAttemptStack attemptStack;
} parser;

parseResult make_parse_error(parser* p, char* msg) {
    parseResult res;
    res.hasError = true;
    strncpy(res.errorMessage, msg, ERROR_MESSAGE_SIZE);
    sprintf(res.errorMessage, "parser error: %s at line %d, column %d", msg, p->line, p->column);
    return res;
}

parseResult make_parse_result(move* moveTreeRoot) {
    parseResult res;
    res.hasError = false;
    res.moveTreeRoot = moveTreeRoot;
    return res;
}

int read_buffer_initial(parser* p) {
    // fprintf(stderr, "read_buffer_initial()\n");
    p->bufferCursor = 0;
    char* buf = fgets(p->buffer, BUFFER_SIZE, p->file);
    if (buf) {
        // printf("got bufer %s\n", buf);
        p->bufferSize = strlen(p->buffer);
        return p->bufferSize;
    } else {
        return 0;
    }
}

int read_buffer(parser* p) {
    // fprintf(stderr, "read_buffer()\n");
    // copy last n characters to the beginning of the buffer (memory)
    strncpy(p->buffer, p->buffer + p->bufferSize - BACKTRACK_SIZE, BACKTRACK_SIZE);

    // reset cursor
    p->bufferCursor = BACKTRACK_SIZE;

    // write after the memory section enough bytes to fill buffer
    char* buf = fgets(p->buffer + BACKTRACK_SIZE, BUFFER_SIZE-BACKTRACK_SIZE, p->file);
    if (buf != NULL) {
        // fprintf(stderr, "new buffer '%s'\n", p->buffer);
        p->bufferSize = strlen(p->buffer);
        return p->bufferSize;
    } else {
        // fprintf(stderr, "failed to read buffer\n");
        return 0;
    }
    // fprintf(stderr, "got %d bytes\n", num);
}

int read_buffer_if_needed(parser* p) {
    // fprintf(stderr, "read_buffer_if_needed()\n");
    // fprintf(stderr, "buffer cursor %d vs size %d\n", p->bufferCursor, p->bufferSize);
    if (p->bufferCursor == p->bufferSize) {
        return read_buffer(p);
    }
    return -1;
}

readResult read_character(parser* p, char c) {
    readResult res;
    read_buffer_if_needed(p);
    if (p->buffer[p->bufferCursor] == c) {
        p->bufferCursor++;
        p->totalCharacterCount++;
        p->column++;
        if (c == '\n') {
            p->column = 1;
            p->line++;
        }
        res.hasError = false;
        res.character = c;
    } else {
        res.hasError = true;
        sprintf(res.errorMessage, "Expected %c, got '%c'", c, p->buffer[p->bufferCursor]);
    }
    return res;
}

readResult read_one_of_characters(parser* p, char* cs) {
    readResult res;
    read_buffer_if_needed(p);
    char c = p->buffer[p->bufferCursor];
    // fprintf(stderr, "got character %c\n", c);
    for (int i = 0; i < strlen(cs); ++i) {
        char t = cs[i];
        // fprintf(stderr, "target character %c\n", t);
        if (c == t) {
            p->bufferCursor++;
            p->totalCharacterCount++;
            p->column++;
            if (c == '\n') {
                p->column = 1;
                p->line++;
            }
            res.hasError = false;
            res.character = c;
            // fprintf(stderr, "wanted character\n");
            return res;
        }
    }
    // fprintf(stderr, "unwanted character\n");
    res.hasError = true;
    res.actualCharacter = c;
    // this message should be overriden by more specific error messages
    sprintf(res.errorMessage, "Unexpected character '%c'", res.actualCharacter);
    return res;
}

readResult read_digit(parser* p) {
    // fprintf(stderr, "buffer cursor (C): %d\n", p->bufferCursor);
    readResult res = read_one_of_characters(p, "0123456789");
    if (res.hasError) {
        // fprintf(stderr, "digit error\n");
        sprintf(res.errorMessage, "Expected a digit, got '%c'", res.actualCharacter);
        return res;
    }
    // fprintf(stderr, "digit no error\n");
    res.digit = res.character - '0';
    return res;
}

readResult read_integer(parser* p) {
    readResult res = read_digit(p);
    if (res.hasError) {
        sprintf(res.errorMessage, "Expected a number, got '%c'", res.actualCharacter);
        return res;
    }
    res.integer = res.digit;
    // fprintf(stderr, "initial integer is %d\n", res.integer);
    readResult tempRes = read_digit(p);
    while (!tempRes.hasError) {
        // fprintf(stderr, "new digit %d, now integer is %d\n", tempRes.digit, res.integer);
        res.integer *= 10;
        res.integer += tempRes.digit;
        tempRes = read_digit(p);
    }
    return res;
}

readResult read_chess_file(parser* p) {
    readResult res = read_one_of_characters(p, "abcdefgh");
    if (res.hasError) {
        sprintf(res.errorMessage, "Expected a rank (1-8), got '%c'", res.actualCharacter);
        return res;
    }
    switch (res.character) {
        case 'a':
            res.file = aFile;
            break;
        case 'b':
            res.file = bFile;
            break;
        case 'c':
            res.file = cFile;
            break;
        case 'd':
            res.file = dFile;
            break;
        case 'e':
            res.file = eFile;
            break;
        case 'f':
            res.file = fFile;
            break;
        case 'g':
            res.file = gFile;
            break;
        case 'h':
            res.file = hFile;
            break;
        default:
            fprintf(stderr, "unexpected chess file character when converting to enum: %c\n", res.character);
            exit(1);
    }
    res.file = res.character;
    return res;
}

readResult read_rank(parser* p) {
    readResult res = read_digit(p);
    if (res.hasError) {
        // fprintf(stderr, "rank error\n");
        sprintf(res.errorMessage, "Expected a rank, got '%c'", res.actualCharacter);
        return res;
    } else if (res.digit == 9) {
        sprintf(res.errorMessage, "9 is not a valid rank (1-8)");
        return res;
    }
    // fprintf(stderr, "rank no error\n");
    res.rank = res.digit;
    return res;
}

readResult read_piece(parser* p) {
    readResult res = read_one_of_characters(p, "NBRQK");
    if (res.hasError) {
        sprintf(res.errorMessage, "Expected a piece symbol (N,B,R,Q,K), got '%c'", res.actualCharacter);
        return res;
    }
    switch (res.character) {
        case 'N':
            res.piece = knight;
            break;
        case 'B':
            res.piece = bishop;
            break;
        case 'R':
            res.piece = rook;
            break;
        case 'Q':
            res.piece = queen;
            break;
        case 'K':
            res.piece = king;
            break;
        default:
            fprintf(stderr, "Unexpected symbol %c when matching piece symbol\n", res.character);
            break;
    }
    return res;
}

readResult read_capture(parser* p) {
    return read_character(p, 'x');
}

readResult read_check(parser* p) {
    return read_character(p, '+');
}

readResult read_checkmate(parser* p) {
    return read_character(p, '#');
}

readResult read_whitespace(parser* p) {
    // fprintf(stderr, "read_whitespace()\n");
    return read_one_of_characters(p, " \n");
}

readResult read_tab(parser* p) {
    return read_character(p, '\t');
}

readResult read_promotion_symbol(parser* p) {
    return read_character(p, '=');
}

readResult read_castling_symbol(parser* p) {
    return read_character(p, 'O');
}

readResult read_dash(parser* p) {
    return read_character(p, '-');
}

parser make_parser(FILE* file) {
    parser p;
    p.file = file;
    p.state = startState;
    p.line = 1;
    p.column = 1;
    p.moveTree = p.currentMove = mkMove();
    p.currentMove->isRoot = true;
    p.decisionLevel = 0;
    p.totalCharacterCount = 0;
    p.attemptStack.top = -1;
    return p;
}

parseResult parse_until(parser p, parserState untilState) {
    // fprintf(stderr, "parse_until()\n");
    int digit;
    int decisionLevel = 0;
    readResult res;
    move* newMove;
    parserAttempt newAttempt;

    if (read_buffer_initial(&p) <= 0) {
        return make_parse_error(&p, "cannot read file");
    }

    char errorMessage[256];
    sprintf(errorMessage, "");
    bool failed = false;

    while (p.state != untilState) {
        // fprintf(stderr, "State: %s Line: %d Column: %d Buffer offset: %d\n", state_to_string(p.state), p.line, p.column, p.bufferCursor);
        if (strcmp(errorMessage, "") != 0) {
            // got error, see if we need to backtrack
            parserAttempt* att = pop_attempt(&p.attemptStack);
            if (att != NULL) {
                // NOTE: the attempts must not have altered move tree
                int shiftAmount = p.totalCharacterCount - att->initialCharacterCount;
                // fprintf(stderr, "Attempt failed, backtracking to state %s and shifting input by %d\n", state_to_string(att->stateOnFailure), shiftAmount);
                p.state = att->stateOnFailure;
                p.line = att->line;
                p.column = att->column;
                p.bufferCursor -= shiftAmount;
                p.totalCharacterCount -= shiftAmount;
                sprintf(errorMessage, "");
            } else {
                return make_parse_error(&p, errorMessage);
            }
        }
        if (read_buffer_if_needed(&p) == 0) {
            if (p.state != parseIndentationLevelState) {
                sprintf(errorMessage, "unexpected end of file");
                continue;
            }
            p.state = endState;
        }
        switch (p.state) {
            case startState:
                p.state = beginParseMoveState;
                break;
            case beginParseMoveState:
                newMove = mkMove();
                if (!newMove) {
                    sprintf(errorMessage, "failed to allocate memory for new move");
                    continue;
                }
                newMove->decisionLevel = decisionLevel;
                appendMove(p.currentMove, newMove);
                p.currentMove = newMove;
                p.state = parseProbabilityState;
                break;
            case parseProbabilityState:
                res = read_integer(&p);
                if (!res.hasError) {
                    p.currentMove->probability = res.integer;
                    res = read_character(&p, '%');
                    if (res.hasError) {
                        sprintf(errorMessage, "%s", res.errorMessage);
                        continue;
                    }
                    res = read_whitespace(&p);
                    if (res.hasError) {
                        sprintf(errorMessage, "%s", res.errorMessage);
                        continue;
                    }
                }
                p.state = parseAlgebraicNotation;
                break;
            case parseAlgebraicNotation:
                res = read_castling_symbol(&p);
                if (!res.hasError) {
                    res = read_dash(&p);
                    if (!res.hasError) {
                        p.state = parseShortCastlingState;
                    } else {
                        sprintf(errorMessage, "%s", res.errorMessage);
                        continue;
                    }
                } else {
                    p.state = parseDepartureOrDestinationState;
                }
                break;
            case parseShortCastlingState:
                res = read_castling_symbol(&p);
                if (res.hasError) {
                    sprintf(errorMessage, "%s", res.errorMessage);
                    continue;
                }
                res = read_dash(&p);
                if (!res.hasError) {
                    p.state = parseLongCastlingState;
                } else {
                    p.currentMove->piece = king;
                    p.currentMove->isShortCastling = true;
                    p.state = finishParseMoveState;
                }
                break;
            case parseLongCastlingState:
                res = read_castling_symbol(&p);
                if (res.hasError) {
                    sprintf(errorMessage, "%s", res.errorMessage);
                    continue;
                }
                p.currentMove->piece = king;
                p.currentMove->isLongCastling = true;
                p.state = finishParseMoveState;
                break;
            case parseDepartureOrDestinationState:
                // fprintf(stderr, "decision point: departure or destination\n");
                newAttempt.line = p.line;
                newAttempt.column = p.column;
                newAttempt.initialCharacterCount = p.totalCharacterCount;
                newAttempt.stateOnFailure = noDepartureState;
                push_attempt(&p.attemptStack, newAttempt);
                p.state = parseDepartureFileState;
                break;
            case noDepartureState:
                p.currentMove->departurePosition.file = p.currentMove->departurePosition.rank = 0;
                p.state = parseMoveDestinationState;
                break;
            case parseDepartureFileState:
                res = read_chess_file(&p);
                if (!res.hasError) {
                    p.currentMove->departurePosition.file = res.file;
                }
                p.state = parseDepartureRankState;
                break;
            case parseDepartureRankState:
                res = read_rank(&p);
                if (!res.hasError) {
                    p.currentMove->departurePosition.rank = res.rank;
                }
                p.state = parseMovePieceState;
                break;
            case parseMovePieceState:
                res = read_piece(&p);
                if (!res.hasError) {
                    p.currentMove->piece = res.piece;
                } else {
                    p.currentMove->piece = pawn;
                }
                p.state = parseMoveCaptureState;
                break;
            case parseMoveCaptureState:
                res = read_capture(&p);
                if (!res.hasError) {
                    p.currentMove->isCapture = 1;
                }
                p.state = parseMoveDestinationState;
                break;
            case parseMoveDestinationState:
                // fprintf(stderr, "buffer cursor (A): %d\n", p.bufferCursor);
                res = read_chess_file(&p);
                if (!res.hasError) {
                    p.currentMove->destination.file = res.file;
                    // fprintf(stderr, "got file %d\n", res.file);
                } else {
                    sprintf(errorMessage, "%s", res.errorMessage);
                    continue;
                }
                // fprintf(stderr, "buffer cursor (B): %d\n", p.bufferCursor);
                res = read_rank(&p);
                if (!res.hasError) {
                    p.currentMove->destination.rank = res.rank;
                } else {
                    fprintf(stderr, "parsemovedestination error\n");
                    sprintf(errorMessage, "%s", res.errorMessage);
                    continue;
                }
                // no need to go back to this anymore
                pop_attempt(&p.attemptStack);
                p.state = parseMovePromotionState;
                break;
            case parseMovePromotionState:
                res = read_promotion_symbol(&p);
                if (!res.hasError) {
                    res = read_piece(&p);
                    if (!res.hasError) {
                        p.currentMove->promoteTo = res.piece;
                    } else {
                        sprintf(errorMessage, "Expected type of piece to promote to, got '%c'", res.actualCharacter);
                        continue;
                    }
                }
                p.state = parseMoveCheckState;
                break;
            case parseMoveCheckState:
                res = read_check(&p);
                if (!res.hasError) {
                    p.currentMove->isCheck = true;
                    p.state = finishParseMoveState;
                } else {
                    p.state = parseMoveCheckmateState;
                }
                break;
            case parseMoveCheckmateState:
                res = read_checkmate(&p);
                if (!res.hasError) {
                    p.currentMove->isCheckmate = true;
                }
                p.state = finishParseMoveState;
                break;
            case finishParseMoveState:
                p.state = parseWhitespaceState;
                break;
            case parseWhitespaceState:
                res = read_whitespace(&p);
                if (res.hasError) {
                    sprintf(errorMessage, "%s", res.errorMessage);
                    continue;
                }
                while (!res.hasError && res.character != '\n') {
                    res = read_whitespace(&p);
                }
                if (res.character == '\n') {
                    p.state = parseIndentationLevelState;
                } else { // end of whitespace, read character
                    p.state = beginParseMoveState;
                }
                break;
            case parseIndentationLevelState:
                decisionLevel = 0;
                res = read_tab(&p);
                while (!res.hasError) {
                    decisionLevel += 1;
                    res = read_tab(&p);
                }
                while (p.currentMove->decisionLevel != decisionLevel - 1) {
                    p.currentMove = p.currentMove->previousMove; 
                }
                p.decisionLevel = decisionLevel;
                p.state = beginParseMoveState;
                break;
            case endState:
                break;
            default:
                fprintf(stderr, "Unexpected parser state\n");
                exit(1);
                // error
                break;
        }
    }
    return make_parse_result(p.moveTree);
}

parseResult parse_algebraic_notation(FILE* file) {
    parser p = make_parser(file);
    p.state = parseAlgebraicNotation;
    return parse_until(p, finishParseMoveState);
}

parseResult parse_variants(FILE* file) {
    parser p = make_parser(file);
    return parse_until(p, endState);
}

void print_position(position pos) {
    switch (pos.file) {
        case aFile:
            printf("a");
            break;
        case bFile:
            printf("b");
            break;
        case cFile:
            printf("c");
            break;
        case dFile:
            printf("d");
            break;
        case eFile:
            printf("e");
            break;
        case fFile:
            printf("f");
            break;
        case gFile:
            printf("g");
            break;
        case hFile:
            printf("h");
            break;
    }
    if (pos.rank) {
        printf("%d", pos.rank);
    }
}

void print_piece(pieceEnum p) {
    switch (p) {
        case pawn:
            break;
        case knight:
            printf("N");
            break;
        case bishop:
            printf("B");
            break;
        case rook:
            printf("R");
            break;
        case queen:
            printf("Q");
            break;
        case king:
            printf("K");
            break;
    }
}

void print_algebraic_notation(move* m) {
    if (m->departurePosition.file || m->departurePosition.rank) {
        print_position(m->departurePosition);
    }
    print_piece(m->piece);
    if (m->isCapture) {
        printf("x");
    }
    print_position(m->destination);
    if (m->promoteTo != pawn) {
        printf("=");
        print_piece(m->promoteTo);
    }
    if (m->isCheck) {
        printf("+");
    }
    if (m->isCheckmate) {
        printf("#");
    }
}

void print_tree(move* m) {
    if (m->previousMove && m->previousMove->decisionLevel != m->decisionLevel) {
        printf("\n");
        for (int i = 0; i < m->decisionLevel; ++i) {
            printf("\t");
        }
    }
    if (m->probability != 0) {
        printf("%d%% ", m->probability);
    }
    print_algebraic_notation(m);
    printf(" ");
    move*c = m->firstChoice;
    while (c != NULL) {
        print_tree(c);
        c = c->nextChoice;
    }
}

move* choose_move(move* currentMove) {
    // TODO: other choices
    return currentMove->firstChoice;
}

// compares only curent move, not child/parent choices
bool moves_equal(move* m1, move* m2) {
    return m1->departurePosition.rank == m2->departurePosition.rank && m1->departurePosition.file == m2->departurePosition.file && m1->piece == m2->piece && m1->destination.rank == m2->destination.rank && m1->destination.file == m2->destination.file;
}

move* apply_move(move* moveTree, move* newMove) {
    move* c = moveTree->firstChoice;
    while (c != NULL) {
        if (moves_equal(c, newMove)) {
            return c;
        }
        c = c->nextChoice;
    }
    return NULL;
}

void play(move* tree, playerSide userSide) {
    setvbuf(stdin, NULL, _IOLBF, -1);
    move* currentMove = tree;
    while (currentMove != NULL) {
        // printf("currentMove:\n");
        if (!currentMove->isRoot) {
            print_algebraic_notation(currentMove);
            printf("\n");
        }
        // printf("\n");
        while (true) {
            parseResult res = parse_algebraic_notation(stdin);
            // printf("got move:\n");
            // print_algebraic_notation(res.moveTreeRoot);
            move* goToMove = apply_move(currentMove, res.moveTreeRoot);
            if (goToMove == NULL) {
                printf("wrong move! try again:\n");
            } else {
                currentMove = goToMove;
                break;
            }
        }
        currentMove = choose_move(currentMove);
    }
}


int main() {
    FILE* fp = fopen("variants.txt", "r");
    parseResult res = parse_variants(fp);
    if (res.hasError) {
        fprintf(stderr, "%s", res.errorMessage);
    }
    // print_tree(res.moveTreeRoot->firstChoice);
    play(res.moveTreeRoot, white);
}
